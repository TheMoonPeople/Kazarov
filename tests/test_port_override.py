"""port_override: a port's PORT_OVERRIDE definitions match ps2/src units, with or without --long-is-64.

Each case writes a port/src unit and a ps2/src unit under tmp_path and runs the real CLI.
"""

import importlib.util
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parent.parent / 'scripts' / 'build' / 'port_override.py'
MARKER = 'g_retail_marker'


@pytest.mark.parametrize('prototype', ['static int F(int);', 'static int F(int);\nstatic int F(int);'])
@pytest.mark.parametrize('storage', ['static ', ''])
def test_private_callback_with_forward_declarations_links_external_override(tmp_path, prototype, storage):
    compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        pytest.skip('a C++ compiler is required for the linkage regression')
    overrides, ps2, stubs = scenario(tmp_path, 'int x', 'int x')
    write(overrides, 'F(int)\n')
    write(ps2 / 'a.cpp', prototype + f'\n{storage}int F(int x) {{ return x + 7; }}\n'
          'static int F(float x) { return 9; }\n'
          'static int (*table[])(int) = {F};\n'
          'int main() { return table[0](3) == 42 && F(0.0f) == 9 ? 0 : 1; }\n')
    assert check(tmp_path, overrides, ps2, stubs, '--private-functions').returncode == 0
    copied = strip(tmp_path, overrides, ps2, '--private-functions')
    write(tmp_path / 'replacement.cpp', 'int F(int) { return 42; }\n')
    executable = tmp_path / ('callback.exe' if os.name == 'nt' else 'callback')
    result = subprocess.run([compiler, str(tmp_path / 'stripped.cpp'),
                             str(tmp_path / 'replacement.cpp'), '-o', str(executable)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'static int F(float x)' in copied
    assert subprocess.run([str(executable)], capture_output=True).returncode == 0


def test_static_prototype_keeps_inherited_linkage_override_opt_in(tmp_path):
    overrides, ps2, stubs = scenario(tmp_path, 'int x', 'int x')
    write(overrides, 'F(int)\n')
    source = 'static int F(int);\nint F(int x) { return x + 7; }\n'
    write(ps2 / 'a.cpp', source)
    assert check(tmp_path, overrides, ps2, stubs).returncode != 0
    assert source in strip(tmp_path, overrides, ps2)
    assert check(tmp_path, overrides, ps2, stubs, '--private-functions').returncode == 0
    copied = strip(tmp_path, overrides, ps2, '--private-functions')
    assert 'static int F' not in copied
    assert 'return x + 7' not in copied


def test_private_variable_override_is_opt_in(tmp_path):
    overrides, ps2, stubs = scenario(tmp_path, 'int x', 'int x')
    write(overrides, 'state\n')
    write(ps2 / 'a.cpp', 'static int state = 7;\nint Read() { return state; }\n')
    assert check(tmp_path, overrides, ps2, stubs).returncode != 0
    assert 'static int state = 7;' in strip(tmp_path, overrides, ps2)
    assert check(tmp_path, overrides, ps2, stubs, '--private-variables').returncode == 0
    copied = strip(tmp_path, overrides, ps2, '--private-variables')
    assert 'extern int state;' in copied
    assert 'return state;' in copied
    assert 'static' not in copied


def test_private_function_is_not_exported_by_variable_mode(tmp_path):
    overrides, ps2, stubs = scenario(tmp_path, 'int x', 'int x')
    write(overrides, 'F(int)\n')
    write(ps2 / 'a.cpp', 'static void F(int x) { }\n')
    assert check(tmp_path, overrides, ps2, stubs, '--private-variables').returncode != 0
    assert 'static void F' in strip(tmp_path, overrides, ps2, '--private-variables')


def test_private_function_override_is_opt_in_and_keeps_callback_binding(tmp_path):
    overrides, ps2, stubs = scenario(tmp_path, 'int x', 'int x')
    write(overrides, 'F(int)\n')
    source = 'static int F(int x) { return x + 7; }\nstatic int (*table[])(int) = {F};\n'
    write(ps2 / 'a.cpp', source)
    assert check(tmp_path, overrides, ps2, stubs).returncode != 0
    assert source in strip(tmp_path, overrides, ps2)
    assert check(tmp_path, overrides, ps2, stubs, '--private-functions').returncode == 0
    copied = strip(tmp_path, overrides, ps2, '--private-functions')
    assert 'int F(int x);' in copied
    assert 'static int F' not in copied
    assert 'return x + 7' not in copied
    assert 'static int (*table[])(int) = {F};' in copied
    assert (ps2 / 'a.cpp').read_text() == source


def test_private_function_mode_leaves_private_variables_and_unlisted_functions(tmp_path):
    overrides, ps2, stubs = scenario(tmp_path, 'int x', 'int x')
    write(overrides, 'F(int)\nstate\n')
    write(ps2 / 'a.cpp', 'static int state = 7;\nstatic void F(int x) { }\nstatic void G() { }\n')
    assert check(tmp_path, overrides, ps2, stubs, '--private-functions').returncode != 0
    copied = strip(tmp_path, overrides, ps2, '--private-functions')
    assert 'static int state = 7;' in copied
    assert 'static void G() { }' in copied
    assert 'void F(int x);' in copied
    assert check(tmp_path, overrides, ps2, stubs, '--private-functions', '--private-variables').returncode == 0


def test_private_function_mode_updates_cache_and_split_entries(tmp_path):
    overrides, ps2, stubs = scenario(tmp_path, 'int x', 'int x')
    write(overrides, 'F(int)\n')
    write(ps2 / 'a.cpp', 'static void F(int x) { }\n')
    flags = ['--cache', tmp_path / 'cache.json', '--split', tmp_path / 'split']
    assert check(tmp_path, overrides, ps2, stubs, *flags).returncode != 0
    assert check(tmp_path, overrides, ps2, stubs, *flags, '--private-functions').returncode == 0
    assert (tmp_path / 'split/a.cpp.txt').read_text() == 'F(int)\n'
    assert check(tmp_path, overrides, ps2, stubs, *flags).returncode != 0


def test_private_function_replacement_still_needs_external_linkage(tmp_path):
    port = tmp_path / 'port'
    write(port / 'a.cpp', 'PORT_OVERRIDE static void F(int x) { }\n')
    result = run('list', port, '-o', tmp_path / 'overrides.txt')
    assert result.returncode != 0
    assert 'a static definition replaces nothing' in result.stderr


def test_private_function_units_preserve_unrelated_same_name_callbacks_and_cache(tmp_path):
    overrides, ps2, stubs = scenario(tmp_path, 'int x', 'int x')
    write(overrides, 'F(int)\nCallback(int)\n')
    write(ps2 / 'a.cpp', 'int F(int x) { return x; }\n')
    private = 'static int F(int);\nstatic int F(int x) { return x + 7; }\n'
    write(ps2 / 'b.cpp', private)
    write(ps2 / 'c.cpp', 'static int Callback(int x) { return x; }\n')
    flags = ['--cache', tmp_path / 'cache.json', '--split', tmp_path / 'split', '--private-functions']
    assert check(tmp_path, overrides, ps2, stubs, *flags).returncode == 0
    assert (tmp_path / 'split/b.cpp.txt').read_text() == 'F(int)\n'
    assert check(tmp_path, overrides, ps2, stubs, *flags, '--private-function-unit', 'c.cpp').returncode == 0
    assert (tmp_path / 'split/b.cpp.txt').read_text() == ''
    assert (tmp_path / 'split/c.cpp.txt').read_text() == 'Callback(int)\n'
    out = tmp_path / 'b-copy.cpp'
    result = run('strip', '--overrides', tmp_path / 'split/b.cpp.txt', ps2 / 'b.cpp', out)
    assert result.returncode == 0
    assert private in out.read_text()
    assert check(tmp_path, overrides, ps2, stubs, *flags).returncode == 0
    assert (tmp_path / 'split/b.cpp.txt').read_text() == 'F(int)\n'


def test_private_function_unit_filter_cannot_enable_mode_implicitly(tmp_path):
    overrides, ps2, stubs = scenario(tmp_path, 'int x', 'int x')
    result = check(tmp_path, overrides, ps2, stubs, '--private-function-unit', 'a.cpp')
    assert result.returncode != 0
    assert 'requires --private-functions' in result.stderr


def test_private_pointer_and_array_keep_declarations(tmp_path):
    overrides, ps2, stubs = scenario(tmp_path, 'int x', 'int x')
    write(overrides, 'ptr\nslots\n')
    write(ps2 / 'a.cpp', 'static Thing *ptr;\nstatic int slots[6] = {1, 2};\n')
    assert check(tmp_path, overrides, ps2, stubs, '--private-variables').returncode == 0
    copied = strip(tmp_path, overrides, ps2, '--private-variables')
    assert 'extern Thing *ptr;' in copied
    assert 'extern int slots[6];' in copied


def run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True)


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def scenario(tmp_path, port_params, ps2_params):
    port = tmp_path / 'port' / 'src'
    write(port / 'a.cpp', f'PORT_OVERRIDE void A::F({port_params}) {{ }}\n')
    overrides = tmp_path / 'overrides.txt'
    listed = run('list', port, '-o', overrides)
    assert listed.returncode == 0, listed.stderr

    ps2 = tmp_path / 'ps2' / 'src'
    write(ps2 / 'a.cpp', f'void A::F({ps2_params}) {{ {MARKER} = 1; }}\n')
    stubs = tmp_path / 'stubs'
    stubs.mkdir()
    return overrides, ps2, stubs


def check(tmp_path, overrides, ps2, stubs, *flags):
    return run('check', '--overrides', overrides, '--stubs', stubs, *flags, ps2, '-o', tmp_path / 'checked')


def strip(tmp_path, overrides, ps2, *flags):
    out = tmp_path / 'stripped.cpp'
    result = run('strip', '--overrides', overrides, *flags, ps2 / 'a.cpp', out)
    assert result.returncode == 0, result.stderr
    return out.read_text(encoding='utf-8')


def test_long_long_port_does_not_match_long_without_option(tmp_path):
    overrides, ps2, stubs = scenario(tmp_path, 'unsigned long long x', 'unsigned long x')

    checked = check(tmp_path, overrides, ps2, stubs)
    assert checked.returncode != 0
    assert 'no ps2/src unit defines it under that signature' in checked.stderr

    assert MARKER in strip(tmp_path, overrides, ps2)


def test_long_long_port_matches_long_with_option(tmp_path):
    overrides, ps2, stubs = scenario(tmp_path, 'unsigned long long x', 'unsigned long x')

    checked = check(tmp_path, overrides, ps2, stubs, '--long-is-64')
    assert checked.returncode == 0, checked.stderr
    assert (tmp_path / 'checked').exists()

    assert MARKER not in strip(tmp_path, overrides, ps2, '--long-is-64')


@pytest.mark.parametrize('ps2_params', [
    'long x',
    'unsigned long x',
    'long int x',
    'const unsigned long *p',
])
def test_long_spellings_in_ps2_match_long_long_with_option(tmp_path, ps2_params):
    port_params = ps2_params.replace('long', 'long long', 1)
    overrides, ps2, stubs = scenario(tmp_path, port_params, ps2_params)

    assert check(tmp_path, overrides, ps2, stubs, '--long-is-64').returncode == 0
    assert MARKER not in strip(tmp_path, overrides, ps2, '--long-is-64')


@pytest.mark.parametrize('port_params, ps2_params', [
    ('int x', 'long x'),
    ('long long x', 'int x'),
])
def test_int_and_long_still_differ_with_option(tmp_path, port_params, ps2_params):
    overrides, ps2, stubs = scenario(tmp_path, port_params, ps2_params)

    checked = check(tmp_path, overrides, ps2, stubs, '--long-is-64')
    assert checked.returncode != 0
    assert 'no ps2/src unit defines it under that signature' in checked.stderr

    assert MARKER in strip(tmp_path, overrides, ps2, '--long-is-64')


@pytest.mark.parametrize('flags', [[], ['--long-is-64']])
def test_same_int_signature_matches_in_both_modes(tmp_path, flags):
    overrides, ps2, stubs = scenario(tmp_path, 'int x', 'int x')

    assert check(tmp_path, overrides, ps2, stubs, *flags).returncode == 0
    assert MARKER not in strip(tmp_path, overrides, ps2, *flags)


F_MARK, G_MARK, K_MARK = 'f_retail_marker', 'g_retail_marker', 'k_retail_marker'


def split_fixture(tmp_path):
    port = tmp_path / 'port' / 'src'
    write(port / 'a.cpp', 'PORT_OVERRIDE void F(int x) { }\n')
    write(port / 'b.cpp', 'PORT_OVERRIDE void G(int y) { }\n')
    overrides = tmp_path / 'overrides.txt'
    assert run('list', port, '-o', overrides).returncode == 0

    ps2 = tmp_path / 'ps2' / 'src'
    write(ps2 / 'a.cpp', f'void F(int x) {{ {F_MARK} = 1; }}\nvoid G(int y) {{ {G_MARK} = 1; }}\n')
    write(ps2 / 'b.cpp', f'void G(int y) {{ {G_MARK} = 1; }}\nvoid K(int z) {{ {K_MARK} = 1; }}\n')
    write(ps2 / 'sub' / 'c.cpp', f'void K(int z) {{ {K_MARK} = 1; }}\n')
    stubs = tmp_path / 'stubs'
    stubs.mkdir()
    return overrides, ps2, stubs


def check_args(tmp_path, overrides, ps2, stubs):
    return ['check', '--overrides', overrides, '--stubs', stubs, '--split', tmp_path / 'split',
            '--cache', tmp_path / 'cache.json', ps2, '-o', tmp_path / 'checked']


def strip_to(tmp_path, name, overrides, source, *flags):
    out = tmp_path / 'out' / name
    result = run('strip', '--overrides', overrides, *flags, source, out)
    assert result.returncode == 0, result.stderr
    return out.read_bytes()


def split_text(tmp_path, unit):
    return (tmp_path / 'split' / f'{unit}.txt').read_text(encoding='utf-8')


def load_script(monkeypatch):
    monkeypatch.setattr(sys, 'dont_write_bytecode', True)
    spec = importlib.util.spec_from_file_location('port_override', SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def count_parses(monkeypatch, module):
    parses = []
    parse = module.overridden

    def counted(*args, **kwargs):
        parses.append(1)
        return parse(*args, **kwargs)

    monkeypatch.setattr(module, 'overridden', counted)
    return parses


def check_in_process(monkeypatch, module, *args):
    monkeypatch.setattr(sys, 'argv', ['port_override.py', *map(str, args)])
    try:
        return module.main()
    except SystemExit as exit:
        return exit.code


def test_split_files_hold_the_entries_each_unit_matches(tmp_path):
    overrides, ps2, stubs = split_fixture(tmp_path)
    write(overrides, 'G(int)\nF(int)\n')

    checked = run(*check_args(tmp_path, overrides, ps2, stubs))
    assert checked.returncode == 0, checked.stderr

    assert split_text(tmp_path, 'a.cpp') == 'G(int)\nF(int)\n'
    assert split_text(tmp_path, 'b.cpp') == 'G(int)\n'
    assert split_text(tmp_path, 'sub/c.cpp') == ''
    for unit in ('a.cpp', 'b.cpp', 'sub/c.cpp'):
        full = strip_to(tmp_path, f'full-{unit}', overrides, ps2 / unit)
        part = strip_to(tmp_path, f'split-{unit}', tmp_path / 'split' / f'{unit}.txt', ps2 / unit)
        assert full == part
    stripped = strip_to(tmp_path, 'a', overrides, ps2 / 'a.cpp')
    assert F_MARK.encode() not in stripped and G_MARK.encode() not in stripped


def test_split_file_of_a_renamed_definition_strips_identically(tmp_path):
    port = tmp_path / 'port' / 'src'
    write(port / 'r.cpp', 'PORT_OVERRIDE void NewName(int x) { }\n')
    overrides = tmp_path / 'overrides.txt'
    assert run('list', port, '-o', overrides).returncode == 0
    ps2 = tmp_path / 'ps2' / 'src'
    write(ps2 / 'r.cpp', f'void OldName(int x) {{ {F_MARK} = 1; }}\n')
    stubs = tmp_path / 'stubs'
    write(stubs / 'r.hpp', '#define OldName NewName\n')

    checked = run(*check_args(tmp_path, overrides, ps2, stubs))
    assert checked.returncode == 0, checked.stderr
    assert split_text(tmp_path, 'r.cpp') == 'NewName(int)\n'

    full = strip_to(tmp_path, 'full', overrides, ps2 / 'r.cpp', '--stubs', stubs / 'r.hpp')
    part = strip_to(tmp_path, 'part', tmp_path / 'split' / 'r.cpp.txt', ps2 / 'r.cpp', '--stubs', stubs / 'r.hpp')
    assert full == part
    assert F_MARK.encode() not in full


def test_cache_hit_skips_parsing_and_gives_the_same_result(tmp_path, monkeypatch):
    module = load_script(monkeypatch)
    parses = count_parses(monkeypatch, module)
    overrides, ps2, stubs = split_fixture(tmp_path)
    args = check_args(tmp_path, overrides, ps2, stubs)

    assert check_in_process(monkeypatch, module, *args) == 0
    assert len(parses) == 3
    cold = {unit: split_text(tmp_path, unit) for unit in ('a.cpp', 'b.cpp', 'sub/c.cpp')}
    checked = (tmp_path / 'checked').read_bytes()

    parses.clear()
    assert check_in_process(monkeypatch, module, *args) == 0
    assert parses == []
    assert {unit: split_text(tmp_path, unit) for unit in cold} == cold
    assert (tmp_path / 'checked').read_bytes() == checked


@pytest.mark.parametrize('change, reparsed', [('unit', 1), ('stub', 1), ('overrides', 3)])
def test_cache_entry_is_invalidated_by_what_it_depends_on(tmp_path, monkeypatch, change, reparsed):
    module = load_script(monkeypatch)
    parses = count_parses(monkeypatch, module)
    overrides, ps2, stubs = split_fixture(tmp_path)
    args = check_args(tmp_path, overrides, ps2, stubs)
    assert check_in_process(monkeypatch, module, *args) == 0
    parses.clear()

    if change == 'unit':
        write(ps2 / 'b.cpp', f'void G(int y) {{ {G_MARK} = 2; }}\nvoid K(int z) {{ {K_MARK} = 1; }}\n')
    elif change == 'stub':
        write(stubs / 'b.hpp', '#define Unused Other\n')
    else:
        write(overrides, 'G(int)\nF(int)\n')

    assert check_in_process(monkeypatch, module, *args) == 0
    assert len(parses) == reparsed


def test_unmatched_override_fails_as_before_with_the_new_flags(tmp_path):
    overrides, ps2, stubs = split_fixture(tmp_path)
    write(overrides, 'F(int)\nG(int)\nNope(int)\n')

    plain = run('check', '--overrides', overrides, '--stubs', stubs, ps2, '-o', tmp_path / 'plain')
    assert plain.returncode != 0
    assert 'no ps2/src unit defines it under that signature' in plain.stderr

    for _ in range(2):
        flagged = run(*check_args(tmp_path, overrides, ps2, stubs))
        assert flagged.returncode == plain.returncode
        assert flagged.stderr == plain.stderr
    assert not (tmp_path / 'checked').exists()
    assert not (tmp_path / 'split').exists()


def test_parse_error_fails_as_before_and_is_reproduced_from_the_cache(tmp_path):
    overrides, ps2, stubs = split_fixture(tmp_path)
    write(ps2 / 'bad.cpp', 'int z = 1 \\\n;\n')

    plain = run('check', '--overrides', overrides, '--stubs', stubs, ps2, '-o', tmp_path / 'plain')
    assert plain.returncode != 0
    assert 'a line continued with a backslash' in plain.stderr

    for _ in range(2):
        flagged = run(*check_args(tmp_path, overrides, ps2, stubs))
        assert flagged.returncode == plain.returncode
        assert flagged.stderr == plain.stderr
    assert not (tmp_path / 'checked').exists()
    assert not (tmp_path / 'split').exists()


def test_outputs_keep_their_mtime_when_unchanged(tmp_path):
    overrides, ps2, stubs = split_fixture(tmp_path)
    args = check_args(tmp_path, overrides, ps2, stubs)
    assert run(*args).returncode == 0

    paths = [tmp_path / 'checked', tmp_path / 'cache.json', *(tmp_path / 'split').rglob('*.txt')]
    stamp = 10 ** 18
    for path in paths:
        os.utime(path, ns=(stamp, stamp))

    assert run(*args).returncode == 0
    for path in paths:
        assert path.stat().st_mtime_ns == stamp, path
