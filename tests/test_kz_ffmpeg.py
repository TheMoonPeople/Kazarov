"""kz-ffmpeg: the LGPL configure line, the build stamp, and the refusals."""

import hashlib
from http.client import IncompleteRead
import io
import json
import os
import runpy
import shlex
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

BIN = Path(__file__).resolve().parent.parent / 'scripts' / 'build'
KZ_FFMPEG = Path(__file__).resolve().parent.parent / 'tools' / 'ffmpeg' / 'kz-ffmpeg'
KZ_FFMPEG_LAUNCHER = BIN / 'kz-ffmpeg'
LICENSE_LINE = 'License: LGPL version 2.1 or later'
GPL_LICENSE_LINE = 'License: GPL version 2 or later'
FLAGS = [
    '--disable-everything',
    '--disable-programs',
    '--enable-ffmpeg',
    '--disable-doc',
    '--disable-network',
    '--disable-autodetect',
    '--enable-avformat',
    '--enable-avfilter',
    '--enable-swscale',
    '--enable-swresample',
    '--disable-avdevice',
    '--enable-decoder=mpeg2video,h264,alac,aac,pcm_s16le',
    '--enable-parser=mpegvideo,h264,aac',
    '--enable-encoder=alac',
    '--enable-demuxer=mpegps,mov,wav,mpegvideo',
    '--enable-muxer=mp4',
    '--enable-protocol=file',
    '--enable-filter=aresample,aformat,anull,abuffer,abuffersink',
    '--enable-pic',
    '--disable-shared',
    '--enable-static',
]
GPL_FLAGS = ['--enable-gpl', '--enable-nonfree', '--enable-version3']

CONFIGURE_SH = '''#!/bin/sh
echo configured >> "$KZ_FAKE_FFMPEG_LOG"
echo "@LICENSE@"
for arg in "$@"; do
  case "$arg" in
    --prefix=*) prefix=${arg#--prefix=} ;;
  esac
done
printf 'all:\\n\\t@true\\n\\ninstall:\\n\\tmkdir -p %s/lib %s/include/libavcodec %s/bin\\n\\ttouch %s/lib/libavcodec.a %s/lib/libavformat.a %s/lib/libavutil.a %s/lib/libswresample.a %s/lib/libswscale.a %s/include/libavcodec/avcodec.h %s/bin/ffmpeg\\n' "$prefix" "$prefix" "$prefix" "$prefix" "$prefix" "$prefix" "$prefix" "$prefix" "$prefix" "$prefix" > Makefile
'''

needs_make = pytest.mark.skipif(os.name == 'nt', reason='needs sh and make')
requires_make = pytest.mark.skipif(shutil.which('make') is None, reason='needs make')


def _ff(*args, log=None):
    env = dict(os.environ, KZ_FAKE_FFMPEG_LOG=str(log)) if log is not None else None
    return subprocess.run([sys.executable, str(KZ_FFMPEG), *map(str, args)],
                          capture_output=True, text=True, env=env)


def _print_configure():
    return subprocess.run([sys.executable, str(KZ_FFMPEG), '--print-configure'],
                          capture_output=True, text=True).stdout


def _fake_tarball(tmp_path, license_line):
    script = CONFIGURE_SH.replace('@LICENSE@', license_line).encode()
    path = tmp_path / 'ffmpeg-9.0.2.tar.xz'
    with tarfile.open(path, 'w:xz') as tf:
        info = tarfile.TarInfo('ffmpeg-9.0.2/configure')
        info.size = len(script)
        info.mode = 0o755
        tf.addfile(info, io.BytesIO(script))
    return path, hashlib.sha256(path.read_bytes()).hexdigest()


def test_configure_line_is_lgpl():
    result = subprocess.run([sys.executable, str(KZ_FFMPEG), '--print-configure'],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    tokens = shlex.split(result.stdout)
    assert tokens[0] == 'configure'
    for flag in FLAGS:
        assert flag in tokens
    for flag in GPL_FLAGS:
        assert flag not in tokens


def test_switch_configure_is_lgpl_libraries_only():
    result = _ff('--target', 'switch-arm64', '--print-configure')
    assert result.returncode == 0, result.stderr
    tokens = shlex.split(result.stdout)
    assert '--enable-ffmpeg' not in tokens
    assert '--enable-cross-compile' in tokens
    assert '--target-os=none' in tokens
    assert '--arch=aarch64' in tokens
    assert '--cross-prefix=aarch64-none-elf-' in tokens
    assert all(flag not in tokens for flag in GPL_FLAGS)
    assert any(flag.startswith('--extra-cflags=') and '-D__SWITCH__' in flag for flag in tokens)


@needs_make
@pytest.mark.skipif(shutil.which('cmake') is None, reason='needs CMake')
def test_switch_headers_precede_toolchain_ffmpeg(tmp_path):
    selected = tmp_path / 'selected'
    foreign = tmp_path / 'toolchain'
    for prefix, content in [(selected, '#define KZ_TEST_SELECTED_FFMPEG 1\n'),
                            (foreign, '#error Incompatible toolchain FFmpeg headers selected\n')]:
        header = prefix / 'include/libavcodec/avcodec.h'
        header.parent.mkdir(parents=True)
        header.write_text(content)
    (selected / 'lib').mkdir()
    for library in ['avutil', 'swresample', 'swscale', 'avcodec', 'avformat']:
        (selected / f'lib/lib{library}.a').write_bytes(b'!<arch>\n')
    (selected / 'kz-ffmpeg.json').write_text(json.dumps({'license': LICENSE_LINE, 'configure': []}))
    source = tmp_path / 'source'
    source.mkdir()
    (source / 'main.cpp').write_text('#include <libavcodec/avcodec.h>\n'
                                     '#ifndef KZ_TEST_SELECTED_FFMPEG\n#error Missing selected headers\n#endif\n'
                                     'int main() { return 0; }\n')
    module = KZ_FFMPEG.parents[2] / 'cmake/KazarovFfmpeg.cmake'
    (source / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.25)\nproject(ffmpeg_header_order LANGUAGES CXX)\n'
        'set(NINTENDO_SWITCH TRUE)\n'
        f'include_directories(SYSTEM "{(foreign / "include").as_posix()}")\n'
        f'include("{module.as_posix()}")\n'
        'add_library(game_options INTERFACE)\nkz_ffmpeg(game_options INTERFACE)\n'
        'add_executable(check_headers main.cpp)\ntarget_link_libraries(check_headers PRIVATE game_options)\n')
    build = tmp_path / 'build'
    configured = subprocess.run(['cmake', '-S', str(source), '-B', str(build),
                                 f'-DKZ_FFMPEG_DIR={selected}'], capture_output=True, text=True)
    assert configured.returncode == 0, configured.stdout + configured.stderr
    compiled = subprocess.run(['cmake', '--build', str(build), '--parallel', '1'], capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr


@needs_make
@requires_make
def test_switch_stamp_does_not_require_an_executable(tmp_path):
    tarball, sha = _fake_tarball(tmp_path, LICENSE_LINE)
    out, log = tmp_path / 'ff', tmp_path / 'fake.log'
    built = _ff('--target', 'switch-arm64', '--out', out, '--tarball', tarball, '--sha256', sha, log=log)
    assert built.returncode == 0, built.stderr
    (out / 'bin/ffmpeg').unlink()
    assert _ff('--target', 'switch-arm64', '--out', out, '--sha256', sha, '--check').returncode == 0
    assert _ff('--out', out, '--sha256', sha, '--check').returncode != 0


def test_the_launcher_runs_the_image_script():
    launched = subprocess.run([sys.executable, str(KZ_FFMPEG_LAUNCHER), '--print-configure'],
                              capture_output=True, text=True)
    direct = subprocess.run([sys.executable, str(KZ_FFMPEG), '--print-configure'],
                            capture_output=True, text=True)
    assert launched.returncode == 0
    assert launched.stdout == direct.stdout


@needs_make
@requires_make
def test_build_writes_a_stamp_that_round_trips(tmp_path):
    tarball, sha = _fake_tarball(tmp_path, LICENSE_LINE)
    out, log = tmp_path / 'ff', tmp_path / 'fake.log'

    built = _ff('--out', out, '--tarball', tarball, '--sha256', sha, log=log)
    assert built.returncode == 0, built.stderr

    stamp = json.loads((out / 'kz-ffmpeg.json').read_text())
    assert stamp['version'] == '9.0.2'
    assert stamp['sha256'] == sha
    assert stamp['configure'] == shlex.split(_print_configure())[1:]
    assert stamp['license'] == LICENSE_LINE

    assert _ff('--out', out, '--sha256', sha, '--check').returncode == 0

    again = _ff('--out', out, '--tarball', tarball, '--sha256', sha, log=log)
    assert again.returncode == 0
    assert 'up to date' in again.stdout
    assert log.read_text().splitlines() == ['configured']


@needs_make
@requires_make
def test_a_changed_stamp_rebuilds(tmp_path):
    tarball, sha = _fake_tarball(tmp_path, LICENSE_LINE)
    out, log = tmp_path / 'ff', tmp_path / 'fake.log'
    build = ['--out', out, '--tarball', tarball, '--sha256', sha]
    assert _ff(*build, log=log).returncode == 0

    stamp_path = out / 'kz-ffmpeg.json'
    good = json.loads(stamp_path.read_text())
    tampered = dict(good, configure=[*good['configure'], '--enable-gpl'])
    stamp_path.write_text(json.dumps(tampered))

    assert _ff('--out', out, '--sha256', sha, '--check').returncode == 1
    assert _ff(*build, log=log).returncode == 0
    assert log.read_text().splitlines() == ['configured', 'configured']
    assert json.loads(stamp_path.read_text()) == good


@needs_make
@requires_make
def test_a_wrong_checksum_is_refused(tmp_path):
    tarball, _ = _fake_tarball(tmp_path, LICENSE_LINE)
    out, log = tmp_path / 'ff', tmp_path / 'fake.log'

    result = _ff('--out', out, '--tarball', tarball, '--sha256', '0' * 64, log=log)

    assert result.returncode != 0
    assert not (out / 'kz-ffmpeg.json').exists()
    assert not log.exists() or 'configured' not in log.read_text()


@needs_make
@requires_make
def test_a_gpl_configure_is_refused(tmp_path):
    tarball, sha = _fake_tarball(tmp_path, GPL_LICENSE_LINE)
    out, log = tmp_path / 'ff', tmp_path / 'fake.log'

    result = _ff('--out', out, '--tarball', tarball, '--sha256', sha, log=log)

    assert result.returncode != 0
    assert not (out / 'kz-ffmpeg.json').exists()


@pytest.mark.skipif(not Path('/opt/kazarov/ffmpeg/kz-ffmpeg.json').exists(),
                    reason='runs in the Kazarov image')
def test_the_image_build_is_lgpl():
    out = Path('/opt/kazarov/ffmpeg')
    result = subprocess.run([sys.executable, str(KZ_FFMPEG), '--check', '--out', str(out)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    lines = (out / 'configure.log').read_text().splitlines()
    assert LICENSE_LINE in [line.rstrip() for line in lines]


@pytest.fixture
def download_helper(monkeypatch):
    helper = runpy.run_path(str(KZ_FFMPEG))
    monkeypatch.setattr(helper['time'], 'sleep', Mock())
    return helper


def test_download_retries_connection_reset_atomically(tmp_path, monkeypatch, download_helper):
    content = b'verified FFmpeg source'
    destination = tmp_path / 'source.tar.xz'
    destination.write_bytes(b'previous verified archive')
    attempts = Mock(side_effect=[ConnectionResetError('connection reset'), io.BytesIO(content)])
    monkeypatch.setattr(download_helper['urllib'].request, 'urlopen', attempts)

    download_helper['download'](destination, hashlib.sha256(content).hexdigest())

    assert destination.read_bytes() == content
    assert not destination.with_name(destination.name + '.partial').exists()
    assert attempts.call_count == 2
    assert all(call.kwargs['timeout'] == 120 for call in attempts.call_args_list)


@pytest.mark.parametrize('failure', [ConnectionResetError('interrupted body'), IncompleteRead(b'partial', 9)])
def test_download_discards_an_interrupted_body(tmp_path, monkeypatch, download_helper, failure):
    class InterruptedBody(io.BytesIO):
        def read(self, size=-1):
            if self.tell():
                raise failure
            return super().read(size)

    content = b'complete archive'
    destination = tmp_path / 'source.tar.xz'
    attempts = Mock(side_effect=[InterruptedBody(b'partial archive'), io.BytesIO(content)])
    monkeypatch.setattr(download_helper['urllib'].request, 'urlopen', attempts)

    download_helper['download'](destination, hashlib.sha256(content).hexdigest())

    assert destination.read_bytes() == content
    assert not destination.with_name(destination.name + '.partial').exists()
    assert attempts.call_count == 2


def test_download_retries_checksum_failure(tmp_path, monkeypatch, download_helper):
    content = b'complete verified archive'
    destination = tmp_path / 'source.tar.xz'
    attempts = Mock(side_effect=[io.BytesIO(b'truncated archive'), io.BytesIO(content)])
    monkeypatch.setattr(download_helper['urllib'].request, 'urlopen', attempts)

    download_helper['download'](destination, hashlib.sha256(content).hexdigest())

    assert destination.read_bytes() == content
    assert attempts.call_count == 2


@pytest.mark.parametrize('failure', ['network', 'checksum'])
def test_download_failure_preserves_verified_archive(tmp_path, monkeypatch, download_helper, failure):
    destination = tmp_path / 'source.tar.xz'
    original = b'previous verified archive'
    destination.write_bytes(original)
    responses = ([ConnectionResetError('connection reset')] * 5 if failure == 'network'
                 else [io.BytesIO(b'unverified source') for _ in range(5)])
    attempts = Mock(side_effect=responses)
    monkeypatch.setattr(download_helper['urllib'].request, 'urlopen', attempts)

    with pytest.raises(SystemExit, match='after five attempts'):
        download_helper['download'](destination, '0' * 64)

    assert destination.read_bytes() == original
    assert not destination.with_name(destination.name + '.partial').exists()
    assert attempts.call_count == 5


def test_native_macos_stamp_tracks_deployment_target(monkeypatch):
    helper = runpy.run_path(str(KZ_FFMPEG))
    stamp = helper['expected_stamp']
    monkeypatch.setitem(stamp.__globals__, 'sys', SimpleNamespace(platform='darwin'))
    monkeypatch.setenv('MACOSX_DEPLOYMENT_TARGET', '14.0')
    baseline = stamp('digest', None)
    monkeypatch.setenv('MACOSX_DEPLOYMENT_TARGET', '27.0')

    assert baseline != stamp('digest', None)
    assert baseline['macos_deployment_target'] == '14.0'
    assert 'macos_deployment_target' not in stamp('digest', 'switch-arm64')
