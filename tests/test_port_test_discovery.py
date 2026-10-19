"""Every shared regression must reach consumers through kz_port_tests."""

import shutil
import subprocess
from pathlib import Path

import pytest


def test_consumer_discovers_all_shared_regressions(tmp_path):
    if not shutil.which('cmake') or not shutil.which('ninja'):
        pytest.skip('cmake and ninja are required')
    root = Path(__file__).resolve().parent.parent
    (tmp_path / 'empty.cpp').write_text('int fixture;\n')
    (tmp_path / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.28)\n'
        'project(discovery LANGUAGES CXX)\n'
        f'include("{root.as_posix()}/cmake/KazarovPort.cmake")\n'
        'add_library(shared_tests STATIC empty.cpp)\n'
        'kz_port_tests(shared_tests)\n'
        'get_target_property(sources shared_tests SOURCES)\n'
        'list(JOIN sources "\\n" listed)\n'
        'file(WRITE "${CMAKE_BINARY_DIR}/sources.txt" "${listed}\\n")\n')
    build = tmp_path / 'build'
    result = subprocess.run(['cmake', '-G', 'Ninja', '-S', str(tmp_path), '-B', str(build)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    listed = {Path(line).resolve() for line in (build / 'sources.txt').read_text().splitlines()}
    missing = {path.resolve() for path in (root / 'src/cpp/tests').rglob('*_test.cpp')} - listed
    assert not missing, f'shared regressions absent from kz_port_tests: {sorted(missing)}'
