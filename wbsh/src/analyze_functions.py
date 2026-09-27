import sys
import re
from pathlib import Path

def find_functions(filepath):
    """Find all functions in a C++ file."""
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        lines = f.readlines()
    
    functions = []
    i = 0
    while i < len(lines):
        line = lines[i]
        
        # Skip comments, blank lines, includes, namespace declarations
        stripped = line.strip()
        if (not stripped or stripped.startswith('//') or 
            stripped.startswith('/*') or stripped.startswith('*') or
            'include' in stripped or stripped.startswith('namespace') or
            stripped.startswith('struct ') or stripped.startswith('class ') or
            stripped.startswith('enum') or stripped.startswith('union') or
            stripped.startswith('#')):
            i += 1
            continue
        
        # Look for function signatures: type name(args) followed eventually by {
        # Match patterns like: static void foo(), int bar(), auto baz(), etc.
        if re.search(r'(static\s+)?\w+[\s*&]+\w+\s*\(', line) and '{' in line:
            # Single-line function definition
            func_start = i
            
            # Count braces to find the end
            brace_count = 0
            j = i
            found_open = False
            while j < len(lines):
                for char in lines[j]:
                    if char == '{':
                        brace_count += 1
                        found_open = True
                    elif char == '}':
                        brace_count -= 1
                        if found_open and brace_count == 0:
                            functions.append({
                                'start': func_start + 1,
                                'end': j + 1,
                                'lines': j - func_start + 1,
                                'signature': line.strip()
                            })
                            i = j + 1
                            break
                j += 1
            else:
                i += 1
        elif re.search(r'(static\s+)?\w+[\s*&]+\w+\s*\(', line) and '{' not in line:
            # Multi-line function signature - need to find opening brace
            func_start = i
            j = i
            found_brace = False
            while j < len(lines) and not found_brace:
                if '{' in lines[j]:
                    found_brace = True
                j += 1
            
            if found_brace:
                # Now find the closing brace
                brace_count = 1
                j -= 1  # Move back to the line with opening brace
                while j < len(lines) and brace_count > 0:
                    for char in lines[j]:
                        if char == '{':
                            brace_count += 1
                        elif char == '}':
                            brace_count -= 1
                    j += 1
                
                functions.append({
                    'start': func_start + 1,
                    'end': j,
                    'lines': j - func_start,
                    'signature': line.strip()
                })
                i = j
            else:
                i += 1
        else:
            i += 1
    
    return functions

files = [
    'coreutils.cpp', 'executor.cpp', 'awk.cpp', 'builtins.cpp',
    'expander.cpp', 'lineedit.cpp', 'parser.cpp', 'lexer.cpp',
    'repl.cpp', 'pathconv.cpp', 'script.cpp', 'setup.cpp',
    'printer.cpp', 'environment.cpp', 'inflate.cpp', 'main.cpp'
]

for fname in files:
    funcs = find_functions(fname)
    print(f"\n{fname}: {len(funcs)} functions found")
    
    # Show functions > 60 lines
    long_funcs = [f for f in funcs if f['lines'] > 60]
    for f in sorted(long_funcs, key=lambda x: x['lines'], reverse=True)[:5]:
        print(f"  {f['start']}-{f['end']}: {f['lines']} lines - {f['signature'][:70]}")
