"""Build a pinned, minimal FFmpeg for XMA audio (and WMV movies).

Adapted from The Darkness Recomp's tools/build_xma_codec.py (GPLv3): FFmpeg
n8.1.2 with an opt-in `tenchu_raw_frames` option on the XMA decoders that
returns every decoded 512-sample frame of a single stream, without the
container FIFO delay, priming skip or synthesized EOF tail. Upstream codec math
is unchanged. Everything else is stock: the ASF demuxer and WMV/WMA decoders
serve the title's movies.

Requires MSYS2 with mingw-w64-x86_64-gcc and make (TENCHU_MSYS2, ../_Tools/msys64 or
C:/msys64) and
llvm-lib from Visual Studio. Output: build/deps/ffmpeg (bin, include, lib).
"""
from pathlib import Path
import argparse
import glob
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT.parent / '_Tools'
MSYS = next((path for path in ([Path(os.environ['TENCHU_MSYS2'])] if 'TENCHU_MSYS2' in os.environ else [])
             + [TOOLS / 'msys64', Path('C:/msys64')] if (path / 'usr/bin/bash.exe').exists()), TOOLS / 'msys64')
DEPS = ROOT / 'build' / 'deps'
COMMIT = '1c2c67c0b9f7f66ab32c19dcf7f227bcd290aa4c'  # FFmpeg n8.1.2
ARCHIVE_HASH = '1291ae49c285f7bd55c7c059aa43f1a0fd784a1ae22d5c76297dcd11c531248a'
SOURCE_HASH = '803547a38dea1294891c00402d6b3576a16053b0f00b395768c4983740c86553'
SUFFIX = '-tenchu'


def patch_decoder(text: str) -> str:
    def replace(anchor: str, replacement: str):
        nonlocal text
        if text.count(anchor) != 1:
            raise RuntimeError(f'Patch anchor not unique: {anchor!r}')
        text = text.replace(anchor, replacement)

    replace('#include "libavutil/mem.h"', '#include "libavutil/mem.h"\n#include "libavutil/opt.h"')
    replace('typedef struct XMADecodeCtx {', 'typedef struct XMADecodeCtx {\n    const AVClass *class;\n    int raw_frames;')
    anchor = '    int i, ret = 0, eof = 0;'
    replace(anchor, anchor + '''
    /* Tenchu recomp opt-in: expose every decoded 512-sample frame of the single
     * stream. The emulated XMA context owns buffering; empty input is not a
     * request for a synthetic tail. */
    if (s->raw_frames) {
        if (!avpkt->size) { *got_frame_ptr = 0; return 0; }
        frame->nb_samples = 512;
        if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) return ret;
        return decode_packet(avctx, &s->xma[0], frame, got_frame_ptr, avpkt);
    }
''')
    anchor = '    /* init all streams (several streams of 1/2ch make Nch files) */'
    replace(anchor, '    if (s->raw_frames && s->num_streams != 1)\n        return AVERROR(EINVAL);\n' + anchor)
    replace('        s->frames[i] = av_frame_alloc();',
            '        if (s->raw_frames) s->xma[i].skip_frame = 0;\n        s->frames[i] = av_frame_alloc();')
    replace('    s->current_stream = 0;\n    s->flushed = 0;',
            '    if (s->raw_frames) s->xma[0].skip_frame = 0;\n    s->current_stream = 0;\n    s->flushed = 0;')
    options = '''static const AVOption tenchu_xma_options[] = {
    { "tenchu_raw_frames", "Return raw XMA frames without container delay or trimming",
      offsetof(XMADecodeCtx, raw_frames), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1,
      AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_DECODING_PARAM },
    { NULL }
};
static const AVClass tenchu_xma_class = {
    .class_name = "Tenchu XMA",
    .item_name = av_default_item_name,
    .option = tenchu_xma_options,
    .version = LIBAVUTIL_VERSION_INT,
};

'''
    replace('const FFCodec ff_xma1_decoder = {', options + 'const FFCodec ff_xma1_decoder = {')
    text = text.replace('    .priv_data_size = sizeof(XMADecodeCtx),',
                        '    .p.priv_class   = &tenchu_xma_class,\n    .priv_data_size = sizeof(XMADecodeCtx),')
    return text


def llvm_lib() -> str:
    for pattern in ['C:/Program Files/LLVM/bin/llvm-lib.exe',
                    'C:/Program Files*/Microsoft Visual Studio/*/*/VC/Tools/Llvm/x64/bin/llvm-lib.exe']:
        found = glob.glob(pattern)
        if found:
            return found[0]
    raise RuntimeError('llvm-lib.exe not found (install the Visual Studio Clang tools)')


def msys_path(path: Path) -> str:
    text = path.resolve().as_posix()
    return '/' + text[0].lower() + text[2:]


def main(rebuild: bool):
    if not (MSYS / 'usr/bin/bash.exe').exists():
        raise SystemExit('MSYS2 was not found (TENCHU_MSYS2, ../_Tools/msys64 or C:/msys64): install it from '
                         'https://www.msys2.org and run: pacman -S mingw-w64-x86_64-gcc make')
    DEPS.mkdir(parents=True, exist_ok=True)
    archive = DEPS / 'ffmpeg-upstream.tar.gz'
    if not archive.exists():
        urllib.request.urlretrieve(f'https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/{COMMIT}', archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_HASH:
        raise RuntimeError('Pinned FFmpeg archive hash mismatch')
    source = DEPS / ('FFmpeg-' + COMMIT)
    with tarfile.open(archive) as tar:
        if not source.exists():
            tar.extractall(DEPS, filter='data')
        original = tar.extractfile(f'FFmpeg-{COMMIT}/libavcodec/wmaprodec.c').read()
    if hashlib.sha256(original).hexdigest() != SOURCE_HASH:
        raise RuntimeError('Pinned decoder source hash mismatch')
    patched = patch_decoder(original.decode()).encode()
    decoder = source / 'libavcodec' / 'wmaprodec.c'
    if decoder.read_bytes() not in (original, patched):
        raise RuntimeError('Unexpected local edits to wmaprodec.c; refusing to overwrite')
    decoder.write_bytes(patched)

    build = DEPS / 'ffmpeg-build'
    install = DEPS / 'ffmpeg'
    build.mkdir(exist_ok=True)
    configure = [msys_path(source / 'configure'), '--prefix=' + msys_path(install), '--target-os=mingw32',
                 '--arch=x86_64', '--cc=gcc', '--disable-everything', '--disable-autodetect',
                 '--disable-programs', '--disable-doc', '--disable-network', '--disable-debug',
                 '--disable-avdevice', '--disable-avfilter', '--disable-swscale', '--disable-swresample',
                 '--disable-asm', '--disable-pthreads', '--enable-w32threads',
                 '--enable-avcodec', '--enable-avutil', '--enable-avformat',
                 '--enable-decoder=xma1,xma2,wmv1,wmv2,wmv3,vc1,wmav1,wmav2,wmapro',
                 '--enable-demuxer=asf', '--enable-parser=vc1', '--enable-protocol=file',
                 '--enable-shared', '--disable-static', '--build-suffix=' + SUFFIX,
                 '--extra-ldflags=-static-libgcc']
    script = 'set -eu\nexport PATH=/mingw64/bin:/usr/bin\ncd ' + shlex.quote(msys_path(build)) + '\n'
    stamp = build / 'configure-args.json'
    if rebuild or not stamp.exists() or json.loads(stamp.read_text()) != configure:
        script += ' '.join(map(shlex.quote, configure)) + '\n'
    if rebuild:
        script += 'make clean\n'
    script += 'make -j8\nmake install\n'
    subprocess.run([str(MSYS / 'usr/bin/bash.exe'), '-lc', script], check=True)
    stamp.write_text(json.dumps(configure))

    # MSVC import libraries for the DLLs.
    tool = llvm_lib()
    for name, version in [('avcodec', 62), ('avutil', 60), ('avformat', 62)]:
        exports = build / ('lib' + name) / f'{name}{SUFFIX}-{version}.def'
        definition = install / 'lib' / f'{name}{SUFFIX}-import.def'
        definition.write_text(f'LIBRARY {name}{SUFFIX}-{version}.dll\n' + exports.read_text())
        subprocess.run([tool, '/machine:x64', '/def:' + str(definition),
                        '/out:' + str(install / 'lib' / f'{name}{SUFFIX}.lib')], check=True)
    shutil.copy2(MSYS / 'mingw64/bin/libwinpthread-1.dll', install / 'bin')
    for filename in ['COPYING.LGPLv2.1', 'LICENSE.md']:
        shutil.copy2(source / filename, install / filename)
    shutil.copy2(MSYS / 'mingw64/share/licenses/winpthreads/COPYING', install / 'COPYING.winpthreads')
    # The MinGW-w64 runtime is linked into the DLLs.
    shutil.copy2(MSYS / 'mingw64/share/licenses/crt/COPYING.MinGW-w64-runtime.txt',
                 install / 'COPYING.MinGW-w64-runtime.txt')
    (install / 'PROVENANCE.json').write_text(json.dumps({
        'upstream_commit': COMMIT, 'archive_sha256': ARCHIVE_HASH,
        'patched_decoder_sha256': hashlib.sha256(patched).hexdigest(),
        'configure': configure, 'license': 'LGPL-2.1-or-later'}, indent=2) + '\n')
    print('FFmpeg ready in', install)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rebuild', action='store_true', help='rebuild all objects')
    main(parser.parse_args().rebuild)
