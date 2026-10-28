#ifndef _LLM_H
#define _LLM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int dim;        /* Transformer dimension */
    int hidden_dim; /* For FFN layers */
    int n_layers;   /* Number of layers */
    int n_heads;    /* Number of query heads */
    int n_kv_heads; /* Number of key/value heads (can be < n_heads for GQA) */
    int vocab_size; /* Vocabulary size */
    int seq_len;    /* Max sequence length */
} llm_config_t;

/* Quantized tensor for INT8 weights */
typedef struct {
    int8_t* q;    /* Quantized values */
    float* s;     /* Scaling factors */
} quant_tensor_t;

typedef struct {
    /* Token embedding table */
    float* token_embedding_table; /* (vocab_size, dim) */
    /* Weights for rmsnorms */
    float* rms_att_weight; /* (layer, dim) */
    float* rms_ffn_weight; /* (layer, dim) */
    /* Weights for matmuls (FP32) */
    float* wq; /* (layer, dim, n_heads * head_size) */
    float* wk; /* (layer, dim, n_kv_heads * head_size) */
    float* wv; /* (layer, dim, n_kv_heads * head_size) */
    float* wo; /* (layer, n_heads * head_size, dim) */
    float* w1; /* (layer, dim, hidden_dim) */
    float* w2; /* (layer, hidden_dim, dim) */
    float* w3; /* (layer, dim, hidden_dim) */
    /* Final rmsnorm */
    float* rms_final_weight; /* (dim,) */
    /* Classifier / lm_head */
    float* wcls; /* (vocab_size, dim) */

    /* Quantized weights (if is_quantized == true) */
    bool is_quantized;
    quant_tensor_t q_tokens;
    quant_tensor_t q_wq;
    quant_tensor_t q_wk;
    quant_tensor_t q_wv;
    quant_tensor_t q_wo;
    quant_tensor_t q_w1;
    quant_tensor_t q_w2;
    quant_tensor_t q_w3;
    quant_tensor_t q_wcls;
} llm_weights_t;

typedef struct {
    /* Activation state */
    float* x;      /* Current activation (dim,) */
    float* xb;     /* Inside residual branch (dim,) */
    float* xb2;    /* Additional buffer (dim,) */
    float* hb;     /* FFN hidden state (hidden_dim,) */
    float* hb2;    /* FFN hidden state 2 (hidden_dim,) */
    float* q;      /* Query (dim,) */
    float* k;      /* Key (dim,) */
    float* v;      /* Value (dim,) */
    float* att;    /* Attention scores (n_heads, seq_len) */
    float* logits; /* Output logits (vocab_size,) */
    /* KV cache */
    float* key_cache;   /* (n_layers, seq_len, kv_dim) */
    float* value_cache; /* (n_layers, seq_len, kv_dim) */
} llm_run_state_t;

typedef struct {
    char** vocab;
    float* vocab_scores;
    int vocab_size;
    unsigned int max_token_length;
    unsigned char byte_pieces[256 * 2];
} llm_tokenizer_t;

typedef struct {
    llm_config_t config;
    llm_weights_t weights;
    llm_run_state_t state;
    llm_tokenizer_t tokenizer;
    int fd;
    size_t file_size;
    void* data_ptr;
    bool is_loaded;
} llm_model_t;

typedef void (*llm_token_callback_t)(const char* piece, int token, void* udata);

int llm_load(llm_model_t* model, const char* checkpoint_path, const char* tokenizer_path);
int llm_generate(llm_model_t* model, const char* prompt, int max_tokens, float temperature,
                 unsigned long long seed, llm_token_callback_t cb, void* udata);
void llm_free(llm_model_t* model);

#ifdef __cplusplus
}
#endif

#endif /* _LLM_H */

