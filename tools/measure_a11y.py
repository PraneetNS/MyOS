def format_node(node, depth=0):
    indent = '  ' * depth
    s = f'{indent}[{node["id"]}] {node["role"]}'
    if node.get('name'):
        s += f' "{node["name"]}"'
    if node.get('val'):
        s += f' val="{node["val"]}"'
    if node.get('states'):
        s += f' states=[{",".join(node["states"])}]'
    if node.get('bounds'):
        s += f' bounds=[{",".join(map(str, node["bounds"]))}]'
    s += '\n'
    for c in node.get('children', []):
        s += format_node(c, depth + 1)
    return s

tab1 = {
    'id': 4, 'role': 'container', 'states': ['enabled'], 'bounds': [10,64,590,320],
    'children': [
        {'id': 5, 'role': 'button', 'name': 'Action Button', 'states': ['enabled'], 'bounds': [30,104,140,32]},
        {'id': 6, 'role': 'button', 'name': 'Show Dialog', 'states': ['enabled'], 'bounds': [190,104,140,32]},
        {'id': 7, 'role': 'label', 'name': 'Checkboxes & Options:', 'val': 'Checkboxes & Options:', 'states': ['enabled'], 'bounds': [30,149,200,20]},
        {'id': 8, 'role': 'checkbox', 'name': 'Enable Hardware Accel', 'states': ['enabled','checked'], 'bounds': [30,174,200,24]},
        {'id': 9, 'role': 'checkbox', 'name': 'Smooth Font Anti-aliasing', 'states': ['enabled'], 'bounds': [30,204,200,24]},
        {'id': 10, 'role': 'label', 'name': 'Radio Options (Theme):', 'val': 'Radio Options (Theme):', 'states': ['enabled'], 'bounds': [310,149,200,20]},
        {'id': 11, 'role': 'radio', 'name': 'Slate Dark Mode', 'states': ['enabled','checked'], 'bounds': [310,174,150,24]},
        {'id': 12, 'role': 'radio', 'name': 'Ocean Blue Light', 'states': ['enabled'], 'bounds': [310,204,150,24]},
        {'id': 13, 'role': 'label', 'name': 'System Progress Bar:', 'val': 'System Progress Bar:', 'states': ['enabled'], 'bounds': [30,249,200,20]},
        {'id': 14, 'role': 'progressbar', 'val': '40%', 'states': ['enabled'], 'bounds': [30,274,540,24]}
    ]
}

tab2 = {
    'id': 15, 'role': 'container', 'states': ['enabled','hidden'], 'bounds': [10,64,590,320],
    'children': [
        {'id': 16, 'role': 'label', 'name': 'Single-line Input (TextBox):', 'val': 'Single-line Input (TextBox):', 'states': ['enabled'], 'bounds': [30,79,250,20]},
        {'id': 17, 'role': 'textbox', 'val': 'Hello MyOS libui!', 'states': ['enabled'], 'bounds': [30,104,540,32]},
        {'id': 18, 'role': 'label', 'name': 'Multi-line Input (TextArea):', 'val': 'Multi-line Input (TextArea):', 'states': ['enabled'], 'bounds': [30,149,250,20]},
        {'id': 19, 'role': 'textarea', 'val': 'Welcome to MyOS Stage 18B-2.\nWidgets are rendered in RAM backbuffer.\nSupports line breaks and fast blitting.', 'states': ['enabled'], 'bounds': [30,174,540,140]}
    ]
}

tab3 = {
    'id': 20, 'role': 'container', 'states': ['enabled','hidden'], 'bounds': [10,64,590,320],
    'children': [
        {'id': 21, 'role': 'label', 'name': 'Scrollable List View:', 'val': 'Scrollable List View:', 'states': ['enabled'], 'bounds': [30,79,250,20]},
        {'id': 22, 'role': 'listbox', 'states': ['enabled'], 'bounds': [30,104,300,200], 'children': [
            {'id': 22001, 'role': 'listitem', 'name': 'kernel32.bin (Stage 18B)', 'states': ['enabled'], 'bounds': [30,106,296,20]},
            {'id': 22002, 'role': 'listitem', 'name': 'libwin.a (Display Client)', 'states': ['enabled'], 'bounds': [30,126,296,20]},
            {'id': 22003, 'role': 'listitem', 'name': 'libui.a (Widget Toolkit)', 'states': ['enabled'], 'bounds': [30,146,296,20]},
            {'id': 22004, 'role': 'listitem', 'name': 'term.elf (Terminal)', 'states': ['enabled'], 'bounds': [30,166,296,20]},
            {'id': 22005, 'role': 'listitem', 'name': 'files.elf (File Manager)', 'states': ['enabled'], 'bounds': [30,186,296,20]},
            {'id': 22006, 'role': 'listitem', 'name': 'edit.elf (Text Editor)', 'states': ['enabled'], 'bounds': [30,206,296,20]},
            {'id': 22007, 'role': 'listitem', 'name': 'sysmon.elf (Task Manager)', 'states': ['enabled'], 'bounds': [30,226,296,20]}
        ]}
    ]
}

tree_default = {
    'id': 1, 'role': 'container', 'name': 'root', 'states': ['enabled'], 'bounds': [0,0,620,440],
    'children': [
        {'id': 2, 'role': 'menubar', 'name': 'File', 'states': ['enabled'], 'bounds': [0,0,620,24]},
        {'id': 3, 'role': 'tabcontrol', 'states': ['enabled'], 'bounds': [10,34,600,360], 'children': [tab1]},
        {'id': 23, 'role': 'label', 'name': 'Status: Ready', 'val': 'Status: Ready', 'states': ['enabled'], 'bounds': [20,400,580,24]}
    ]
}

tree_all = {
    'id': 1, 'role': 'container', 'name': 'root', 'states': ['enabled'], 'bounds': [0,0,620,440],
    'children': [
        {'id': 2, 'role': 'menubar', 'name': 'File', 'states': ['enabled'], 'bounds': [0,0,620,24]},
        {'id': 3, 'role': 'tabcontrol', 'states': ['enabled'], 'bounds': [10,34,600,360], 'children': [tab1, tab2, tab3]},
        {'id': 23, 'role': 'label', 'name': 'Status: Ready', 'val': 'Status: Ready', 'states': ['enabled'], 'bounds': [20,400,580,24]}
    ]
}

text_default = format_node(tree_default)
text_all = format_node(tree_all)

print('=== DEFAULT VIEW ===')
print(f'Characters: {len(text_default)}')
print(f'Words: {len(text_default.split())}')
print(f'Tokens (~4 chars/token): {len(text_default)/4.0:.1f} (~{round(len(text_default)/4.0)} tokens)')

print('\n=== ALL TABS EXPANDED ===')
print(f'Characters: {len(text_all)}')
print(f'Words: {len(text_all.split())}')
print(f'Tokens (~4 chars/token): {len(text_all)/4.0:.1f} (~{round(len(text_all)/4.0)} tokens)')

