#include "SafeOpenCmd.h"
#include "PluginLog.h"

#include <maya/MGlobal.h>

const char* SafeOpenCmd::kCommandName = "safeOpenScene";

SafeOpenCmd::SafeOpenCmd() {}
SafeOpenCmd::~SafeOpenCmd() {}

void* SafeOpenCmd::creator() {
    return new SafeOpenCmd();
}

MSyntax SafeOpenCmd::newSyntax() {
    MSyntax syntax;
    return syntax;
}

MStatus SafeOpenCmd::doIt(const MArgList& /*args*/) {
    PluginLog::info("SafeOpen", "doIt: command invoked, scheduling deferred open");

    // We use Python cmds.file() instead of MEL eval+string concatenation
    // because MEL string interpolation corrupts Chinese/Unicode paths on
    // non-UTF-8 Windows systems (ACP=936), causing Maya to crash when
    // opening .mb files.  Python passes the path as a proper argument
    // without re-encoding.
    //
    // Detailed logging is written to PipelineTools.log at every step so
    // that if Maya crashes during file loading we can see exactly where
    // it stopped.
    MString mel =
        "python(\""
        "import os, sys, struct, codecs\\n"
        "import maya.cmds as cmds\\n"
        "\\n"
        "# ---- helper: write directly to PipelineTools.log ----\\n"
        "def _slog(msg):\\n"
        "    import datetime, os\\n"
        "    try:\\n"
        "        try:\\n"
        "            appDir = cmds.internalVar(userAppDir=True)\\n"
        "        except Exception:\\n"
        "            appDir = os.path.join(os.path.expanduser('~'), 'Documents', 'maya')\\n"
        "        logdir = os.path.join(appDir, 'PipelineTools')\\n"
        "        logpath = os.path.join(logdir, 'PipelineTools.log')\\n"
        "        os.makedirs(logdir, exist_ok=True)\\n"
        "        ts = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')\\n"
        "        with open(logpath, 'a', encoding='utf-8') as f:\\n"
        "            f.write('[' + ts + '][Info][SafeOpen] ' + msg + '\\\\n')\\n"
        "            f.flush()\\n"
        "    except Exception:\\n"
        "        pass\\n"
        "\\n"
        "_slog('Step 1: opening file dialog...')\\n"
        "files = cmds.fileDialog2(fileMode=1,"
            " caption='Open Scene Without References',"
            " fileFilter='Maya Files (*.ma *.mb);;All Files (*.*)')\\n"
        "\\n"
        "if not files:\\n"
        "    _slog('Step 1b: user cancelled dialog')\\n"
        "else:\\n"
        "    path = files[0]\\n"
        "    _slog('Step 2: file selected: ' + path)\\n"
        "\\n"
        "    # --- validate file exists ---\\n"
        "    pathEnc = path\\n"
        "    exists = os.path.isfile(pathEnc)\\n"
        "    _slog('Step 3: os.path.isfile = ' + str(exists))\\n"
        "    if not exists:\\n"
        "        cmds.warning('File does not exist: ' + path)\\n"
        "        _slog('ABORT: file not found')\\n"
        "    else:\\n"
        "        # --- file size ---\\n"
        "        fsize = os.path.getsize(pathEnc)\\n"
        "        _slog('Step 4: file size = ' + str(fsize) + ' bytes')\\n"
        "\\n"
        "        # --- detect file type from extension ---\\n"
        "        ext = os.path.splitext(path)[1].lower()\\n"
        "        _slog('Step 5: extension = ' + ext)\\n"
        "\\n"
        "        # --- for .mb: validate magic header ---\\n"
        "        headerOk = True\\n"
        "        if ext == '.mb':\\n"
        "            try:\\n"
        "                with open(pathEnc, 'rb') as bf:\\n"
        "                    hdr = bf.read(16)\\n"
        "                _slog('Step 5b: .mb header (hex) = ' + hdr[:16].hex())\\n"
        "                # Maya binary uses IFF format: first 4 bytes are 'FOR4' or 'FOR8'\\n"
        "                tag = hdr[:4]\\n"
        "                if tag not in (b'FOR4', b'FOR8'):\\n"
        "                    _slog('WARN: .mb header does not start with FOR4/FOR8, tag=' + repr(tag))\\n"
        "                    headerOk = False\\n"
        "                else:\\n"
        "                    _slog('Step 5c: .mb header valid (' + tag.decode('ascii') + ')')\\n"
        "            except Exception as ex:\\n"
        "                _slog('Step 5b: failed to read .mb header: ' + str(ex))\\n"
        "                headerOk = False\\n"
        "\\n"
        "        if not headerOk:\\n"
        "            cmds.warning('File appears to be corrupted or not a valid Maya binary: ' + path)\\n"
        "            _slog('ABORT: invalid .mb header')\\n"
        "        else:\\n"
        "            # --- determine file type arg ---\\n"
        "            typeArg = {}\\n"
        "            if ext == '.ma':\\n"
        "                typeArg = {'type': 'mayaAscii'}\\n"
        "            elif ext == '.mb':\\n"
        "                typeArg = {'type': 'mayaBinary'}\\n"
        "            _slog('Step 6: typeArg = ' + str(typeArg))\\n"
        "\\n"
        "            _slog('Step 7: calling cmds.file(open) ...')\\n"
        "            try:\\n"
        "                cmds.file(path, open=True, force=True,"
                            " loadReferenceDepth='none',"
                            " ignoreVersion=True, prompt=False,"
                            " **typeArg)\\n"
        "                _slog('Step 8: cmds.file(open) returned OK')\\n"
        "                print('Scene opened successfully (references not loaded).')\\n"
        "            except RuntimeError as e:\\n"
        "                _slog('Step 8: cmds.file(open) RuntimeError: ' + str(e))\\n"
        "                cmds.warning('Failed to open scene: ' + path + '. ' + str(e))\\n"
        "            except Exception as e:\\n"
        "                _slog('Step 8: cmds.file(open) Exception: ' + type(e).__name__ + ': ' + str(e))\\n"
        "                cmds.warning('Failed to open scene: ' + path + '. ' + str(e))\\n"
        "\");";

    MGlobal::executeCommandOnIdle(mel);
    return MS::kSuccess;
}
