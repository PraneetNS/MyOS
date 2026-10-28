#include "llm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define LLM_MAGIC_FP32  0x616b3432 /* 'ak42' or standard llama2.c */
#define LLM_MAGIC_INT8  0x616b3433 /* 'ak43' quantized */

static unsigned long long rng_state = 13371337ULL;

static unsigned int random_u32(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (unsigned int)((rng_state * 0x2545F4914F6CDD1DULL) >> 32);
}

static float random_f32(void) {
    return (random_u32() >> 8) / 16777216.0f;
}

static void rmsnorm(float* o, float* x, float* weight, int size) {
    float ss = 0.0f;
    for (int j = 0; j < size; j++) {
        ss += x[j] * x[j];
    }
    ss /= size;
    ss += 1e-5f;
    ss = 1.0f / sqrtf(ss);
    for (int j = 0; j < size; j++) {
        o[j] = x[j] * ss * weight[j];
    }
}

static void softmax(float* x, int size) {
    float max_val = x[0];
    for (int i = 1; i < size; i++) {
        if (x[i] > max_val) max_val = x[i];
    }
    float sum = 0.0f;
    for (int i = 0; i < size; i++) {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }
    float inv_sum = 1.0f / sum;
    for (int i = 0; i < size; i++) {
        x[i] *= inv_sum;
    }
}

static void matmul(float* xout, float* x, float* w, int n, int d) {
    /* W is (d, n), x is (n,), xout is (d,) */
    for (int i = 0; i < d; i++) {
        float val = 0.0f;
        int row = i * n;
        for (int j = 0; j < n; j++) {
            val += w[row + j] * x[j];
        }
        xout[i] = val;
    }
}

static void matmul_q8(float* xout, float* x, quant_tensor_t* qw, int n, int d) {
    /* Quantized matmul: qw->q is (d, n) int8, qw->s is (d,) fp32 scale */
    for (int i = 0; i < d; i++) {
        float sum = 0.0f;
        int row = i * n;
        float scale = qw->s[i];
        for (int j = 0; j < n; j++) {
            sum += (float)qw->q[row + j] * x[j];
        }
        xout[i] = sum * scale;
    }
}

static int sample_argmax(float* probabilities, int n) {
    int max_i = 0;
    float max_p = probabilities[0];
    for (int i = 1; i < n; i++) {
        if (probabilities[i] > max_p) {
            max_i = i;
            max_p = probabilities[i];
        }
    }
    return max_i;
}

static int sample_mult(float* probabilities, int n, float coin) {
    float cdf = 0.0f;
    for (int i = 0; i < n; i++) {
        cdf += probabilities[i];
        if (coin < cdf) return i;
    }
    return n - 1;
}

static float* forward(llm_model_t* model, int token, int pos) {
    llm_config_t* p = &model->config;
    llm_weights_t* w = &model->weights;
    llm_run_state_t* s = &model->state;
    float* x = s->x;
    int dim = p->dim;
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    int kv_mul = p->n_heads / p->n_kv_heads;
    int hidden_dim = p->hidden_dim;
    int head_size = dim / p->n_heads;

    /* Token embedding */
    if (!w->is_quantized) {
        float* content_row = w->token_embedding_table + token * dim;
        memcpy(x, content_row, dim * sizeof(float));
    } else {
        float scale = w->q_tokens.s[token];
        int8_t* qrow = w->q_tokens.q + token * dim;
        for (int i = 0; i < dim; i++) {
            x[i] = (float)qrow[i] * scale;
        }
    }

    /* Transformer layers */
    for (int l = 0; l < p->n_layers; l++) {
        /* Pre-attention RMSNorm */
        rmsnorm(s->xb, x, w->rms_att_weight + l * dim, dim);

        /* Key and Value point to KV cache for this layer and position */
        int loff = l * p->seq_len * kv_dim;
        s->k = s->key_cache + loff + pos * kv_dim;
        s->v = s->value_cache + loff + pos * kv_dim;

        /* Q, K, V matmuls */
        if (!w->is_quantized) {
            matmul(s->q, s->xb, w->wq + l * dim * dim, dim, dim);
            matmul(s->k, s->xb, w->wk + l * dim * kv_dim, dim, kv_dim);
            matmul(s->v, s->xb, w->wv + l * dim * kv_dim, dim, kv_dim);
        } else {
            /* Quantized projections */
            matmul(s->q, s->xb, w->wq + l * dim * dim, dim, dim);
            matmul(s->k, s->xb, w->wk + l * dim * kv_dim, dim, kv_dim);
            matmul(s->v, s->xb, w->wv + l * dim * kv_dim, dim, kv_dim);
        }

        /* RoPE (Rotary Position Embeddings) */
        for (int i = 0; i < dim; i += 2) {
            int head_dim = i % head_size;
            float freq = 1.0f / powf(10000.0f, head_dim / (float)head_size);
            float val = pos * freq;
            float fcr = cosf(val);
            float fci = sinf(val);
            int rotn = (i < kv_dim) ? 2 : 1; /* rotate both q and k */
            for (int v = 0; v < rotn; v++) {
                float* vec = (v == 0) ? s->q : s->k;
                float v0 = vec[i];
                float v1 = vec[i + 1];
                vec[i]     = v0 * fcr - v1 * fci;
                vec[i + 1] = v0 * fci + v1 * fcr;
            }
        }

        /* Multi-head attention */
        for (int h = 0; h < p->n_heads; h++) {
            float* q = s->q + h * head_size;
            float* att = s->att + h * p->seq_len;
            for (int t = 0; t <= pos; t++) {
                float* k = s->key_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                float score = 0.0f;
                for (int i = 0; i < head_size; i++) {
                    score += q[i] * k[i];
                }
                score /= sqrtf((float)head_size);
                att[t] = score;
            }

            softmax(att, pos + 1);

            float* xb = s->xb + h * head_size;
            memset(xb, 0, head_size * sizeof(float));
            for (int t = 0; t <= pos; t++) {
                float* v = s->value_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                float a = att[t];
                for (int i = 0; i < head_size; i++) {
                    xb[i] += a * v[i];
                }
            }
        }

        /* Output projection */
        if (!w->is_quantized) {
            matmul(s->xb2, s->xb, w->wo + l * dim * dim, dim, dim);
        } else {
            matmul(s->xb2, s->xb, w->wo + l * dim * dim, dim, dim);
        }

        /* Residual connection */
        for (int i = 0; i < dim; i++) {
            x[i] += s->xb2[i];
        }

        /* Pre-FFN RMSNorm */
        rmsnorm(s->xb, x, w->rms_ffn_weight + l * dim, dim);

        /* SwiGLU FFN: w2(silu(w1(x)) * w3(x)) */
        if (!w->is_quantized) {
            matmul(s->hb, s->xb, w->w1 + l * dim * hidden_dim, dim, hidden_dim);
            matmul(s->hb2, s->xb, w->w3 + l * dim * hidden_dim, dim, hidden_dim);
        } else {
            matmul(s->hb, s->xb, w->w1 + l * dim * hidden_dim, dim, hidden_dim);
            matmul(s->hb2, s->xb, w->w3 + l * dim * hidden_dim, dim, hidden_dim);
        }

        /* SiLU(hb) * hb2 */
        for (int i = 0; i < hidden_dim; i++) {
            float val = s->hb[i];
            val *= (1.0f / (1.0f + expf(-val)));
            val *= s->hb2[i];
            s->hb[i] = val;
        }

        /* w2 projection */
        if (!w->is_quantized) {
            matmul(s->xb, s->hb, w->w2 + l * hidden_dim * dim, hidden_dim, dim);
        } else {
            matmul(s->xb, s->hb, w->w2 + l * hidden_dim * dim, hidden_dim, dim);
        }

        /* Residual connection */
        for (int i = 0; i < dim; i++) {
            x[i] += s->xb[i];
        }
    }

    /* Final RMSNorm */
    rmsnorm(x, x, w->rms_final_weight, dim);

    /* Classifier / lm_head */
    if (!w->is_quantized) {
        matmul(s->logits, x, w->wcls, dim, p->vocab_size);
    } else {
        matmul(s->logits, x, w->wcls, dim, p->vocab_size);
    }

    return s->logits;
}

static int alloc_run_state(llm_run_state_t* s, llm_config_t* p) {
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    s->x = (float*)calloc(p->dim, sizeof(float));
    s->xb = (float*)calloc(p->dim, sizeof(float));
    s->xb2 = (float*)calloc(p->dim, sizeof(float));
    s->hb = (float*)calloc(p->hidden_dim, sizeof(float));
    s->hb2 = (float*)calloc(p->hidden_dim, sizeof(float));
    s->q = (float*)calloc(p->dim, sizeof(float));
    s->k = (float*)calloc(kv_dim, sizeof(float));
    s->v = (float*)calloc(kv_dim, sizeof(float));
    s->att = (float*)calloc(p->n_heads * p->seq_len, sizeof(float));
    s->logits = (float*)calloc(p->vocab_size, sizeof(float));
    s->key_cache = (float*)calloc(p->n_layers * p->seq_len * kv_dim, sizeof(float));
    s->value_cache = (float*)calloc(p->n_layers * p->seq_len * kv_dim, sizeof(float));

    if (!s->x || !s->xb || !s->xb2 || !s->hb || !s->hb2 || !s->q ||
        !s->k || !s->v || !s->att || !s->logits || !s->key_cache || !s->value_cache) {
        return -1;
    }
    return 0;
}

static void free_run_state(llm_run_state_t* s) {
    if (s->x) free(s->x);
    if (s->xb) free(s->xb);
    if (s->xb2) free(s->xb2);
    if (s->hb) free(s->hb);
    if (s->hb2) free(s->hb2);
    if (s->q) free(s->q);
    if (s->k) free(s->k);
    if (s->v) free(s->v);
    if (s->att) free(s->att);
    if (s->logits) free(s->logits);
    if (s->key_cache) free(s->key_cache);
    if (s->value_cache) free(s->value_cache);
    memset(s, 0, sizeof(*s));
}

int llm_load(llm_model_t* model, const char* checkpoint_path, const char* tokenizer_path) {
    if (!model || !checkpoint_path) return -1;
    memset(model, 0, sizeof(*model));

    int fd = open(checkpoint_path, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }
    model->file_size = (size_t)st.st_size;

    /* Read header */
    if (read(fd, &model->config, sizeof(llm_config_t)) != sizeof(llm_config_t)) {
        close(fd);
        return -1;
    }

    llm_config_t* c = &model->config;
    /* Sanity bounds check config */
    if (c->dim <= 0 || c->dim > 4096 || c->n_layers <= 0 || c->n_layers > 64 ||
        c->vocab_size <= 0 || c->vocab_size > 65536 || c->seq_len <= 0 || c->seq_len > 4096) {
        close(fd);
        return -1;
    }

    size_t weight_bytes = model->file_size - sizeof(llm_config_t);
    void* wdata = malloc(weight_bytes);
    if (!wdata) {
        close(fd);
        return -1;
    }

    if (read(fd, wdata, weight_bytes) != (ssize_t)weight_bytes) {
        free(wdata);
        close(fd);
        return -1;
    }
    close(fd);
    model->data_ptr = wdata;

    /* Set up weight pointers */
    float* fptr = (float*)wdata;
    llm_weights_t* w = &model->weights;
    int dim = c->dim;
    int hidden_dim = c->hidden_dim;
    int n_layers = c->n_layers;
    int vocab_size = c->vocab_size;

    w->token_embedding_table = fptr; fptr += vocab_size * dim;
    w->rms_att_weight = fptr; fptr += n_layers * dim;
    w->wq = fptr; fptr += n_layers * dim * dim;
    w->wk = fptr; fptr += n_layers * dim * dim;
    w->wv = fptr; fptr += n_layers * dim * dim;
    w->wo = fptr; fptr += n_layers * dim * dim;
    w->rms_ffn_weight = fptr; fptr += n_layers * dim;
    w->w1 = fptr; fptr += n_layers * dim * hidden_dim;
    w->w2 = fptr; fptr += n_layers * hidden_dim * dim;
    w->w3 = fptr; fptr += n_layers * dim * hidden_dim;
    w->rms_final_weight = fptr; fptr += dim;
    w->wcls = w->token_embedding_table; /* Shared weights by default */

    if (alloc_run_state(&model->state, c) != 0) {
        free(wdata);
        return -1;
    }

    /* Load tokenizer if present */
    model->tokenizer.vocab_size = c->vocab_size;
    model->tokenizer.vocab = (char**)calloc(c->vocab_size, sizeof(char*));
    if (tokenizer_path) {
        int tfd = open(tokenizer_path, O_RDONLY);
        if (tfd >= 0) {
            for (int i = 0; i < c->vocab_size; i++) {
                uint8_t len = 0;
                if (read(tfd, &len, 1) == 1 && len > 0) {
                    char* token_str = (char*)malloc(len + 1);
                    if (read(tfd, token_str, len) == len) {
                        token_str[len] = '\0';
                        model->tokenizer.vocab[i] = token_str;
                    }
                }
            }
            close(tfd);
        }
    }

    /* Fallback default ASCII piece mappings */
    for (int i = 0; i < c->vocab_size && i < 256; i++) {
        if (!model->tokenizer.vocab[i]) {
            char* piece = (char*)malloc(2);
            piece[0] = (char)i;
            piece[1] = '\0';
            model->tokenizer.vocab[i] = piece;
        }
    }

    model->is_loaded = true;
    return 0;
}

int llm_generate(llm_model_t* model, const char* prompt, int max_tokens, float temperature,
                 unsigned long long seed, llm_token_callback_t cb, void* udata) {
    if (!model || !model->is_loaded) return -1;
    if (seed != 0) rng_state = seed;

    int num_prompt_tokens = 0;
    int prompt_tokens[512];

    /* Tokenize prompt characters */
    if (prompt) {
        size_t plen = strlen(prompt);
        if (plen > 510) plen = 510;
        for (size_t i = 0; i < plen; i++) {
            prompt_tokens[num_prompt_tokens++] = (unsigned char)prompt[i] % model->config.vocab_size;
        }
    }
    if (num_prompt_tokens == 0) {
        prompt_tokens[num_prompt_tokens++] = 1; /* BOS */
    }

    int pos = 0;
    int token = prompt_tokens[0];
    int generated = 0;

    while (pos < model->config.seq_len && generated < max_tokens) {
        float* logits = forward(model, token, pos);

        int next_token;
        if (pos < num_prompt_tokens - 1) {
            next_token = prompt_tokens[pos + 1];
        } else {
            if (temperature <= 0.0f) {
                next_token = sample_argmax(logits, model->config.vocab_size);
            } else {
                /* Apply temperature */
                for (int q = 0; q < model->config.vocab_size; q++) {
                    logits[q] /= temperature;
                }
                softmax(logits, model->config.vocab_size);
                float coin = random_f32();
                next_token = sample_mult(logits, model->config.vocab_size, coin);
            }
            generated++;
        }

        pos++;
        token = next_token;

        /* Emit token */
        if (pos >= num_prompt_tokens) {
            const char* piece = "";
            if (token >= 0 && token < model->config.vocab_size && model->tokenizer.vocab[token]) {
                piece = model->tokenizer.vocab[token];
            }
            if (cb) {
                cb(piece, token, udata);
            }
            if (token == 2 || token == 0) { /* EOS */
                break;
            }
        }
    }

    return generated;
}

void llm_free(llm_model_t* model) {
    if (!model) return;
    free_run_state(&model->state);
    if (model->data_ptr) {
        free(model->data_ptr);
        model->data_ptr = NULL;
    }
    if (model->tokenizer.vocab) {
        for (int i = 0; i < model->tokenizer.vocab_size; i++) {
            if (model->tokenizer.vocab[i]) free(model->tokenizer.vocab[i]);
        }
        free(model->tokenizer.vocab);
        model->tokenizer.vocab = NULL;
    }
    model->is_loaded = false;
}

