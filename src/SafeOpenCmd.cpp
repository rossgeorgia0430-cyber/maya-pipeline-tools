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
    PluginLog::info("SafeOpen", "doIt: command invoked");

    // Execute immediately instead of deferred-idle. This avoids queued callbacks
    // piling up when users open/new/open in quick succession.
    static const char* kPythonScript = R"PY(
import os
import maya.cmds as cmds

def _slog(msg):
    import datetime
    try:
        try:
            appDir = cmds.internalVar(userAppDir=True)
        except Exception:
            appDir = os.path.join(os.path.expanduser('~'), 'Documents', 'maya')
        logdir = os.path.join(appDir, 'PipelineTools')
        logpath = os.path.join(logdir, 'PipelineTools.log')
        os.makedirs(logdir, exist_ok=True)
        ts = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        with open(logpath, 'a', encoding='utf-8') as f:
            f.write('[' + ts + '][Info][SafeOpen] ' + msg + '\n')
            f.flush()
    except Exception:
        pass

busy_flag = '_pipeline_safe_open_busy'
if getattr(cmds, busy_flag, False):
    _slog('ABORT: previous OpenWithoutReference is still running')
    cmds.warning('Open Without References is busy, please retry in a moment.')
else:
    setattr(cmds, busy_flag, True)
    try:
        _slog('Step 1: opening file dialog...')
        files = cmds.fileDialog2(
            fileMode=1,
            caption='Open Scene Without References',
            fileFilter='Maya Files (*.ma *.mb);;All Files (*.*)'
        )

        if not files:
            _slog('Step 1b: user cancelled dialog')
        else:
            path = files[0]
            _slog('Step 2: file selected: ' + path)

            exists = os.path.isfile(path)
            _slog('Step 3: os.path.isfile = ' + str(exists))
            if not exists:
                _slog('ABORT: file not found')
                cmds.warning('File does not exist: ' + path)
            else:
                fsize = os.path.getsize(path)
                _slog('Step 4: file size = ' + str(fsize) + ' bytes')

                ext = os.path.splitext(path)[1].lower()
                _slog('Step 5: extension = ' + ext)

                header_ok = True
                if ext == '.mb':
                    try:
                        with open(path, 'rb') as bf:
                            hdr = bf.read(16)
                        _slog('Step 5b: .mb header (hex) = ' + hdr[:16].hex())
                        tag = hdr[:4]
                        if tag not in (b'FOR4', b'FOR8'):
                            _slog('WARN: .mb header does not start with FOR4/FOR8, tag=' + repr(tag))
                            header_ok = False
                        else:
                            _slog('Step 5c: .mb header valid (' + tag.decode('ascii') + ')')
                    except Exception as ex:
                        _slog('Step 5b: failed to read .mb header: ' + str(ex))
                        header_ok = False

                if not header_ok:
                    _slog('ABORT: invalid .mb header')
                    cmds.warning('File appears corrupted or not a valid Maya binary: ' + path)
                else:
                    type_arg = {}
                    if ext == '.ma':
                        type_arg = {'type': 'mayaAscii'}
                    elif ext == '.mb':
                        type_arg = {'type': 'mayaBinary'}
                    _slog('Step 6: typeArg = ' + str(type_arg))

                    _slog('Step 7: calling cmds.file(open) ...')
                    try:
                        cmds.file(
                            path,
                            open=True,
                            force=True,
                            loadReferenceDepth='none',
                            ignoreVersion=True,
                            prompt=False,
                            **type_arg
                        )
                        _slog('Step 8: cmds.file(open) returned OK')
                    except RuntimeError as ex:
                        _slog('Step 8: cmds.file(open) RuntimeError: ' + str(ex))
                        cmds.warning('Failed to open scene: ' + path + '. ' + str(ex))
                    except Exception as ex:
                        _slog('Step 8: cmds.file(open) Exception: ' + type(ex).__name__ + ': ' + str(ex))
                        cmds.warning('Failed to open scene: ' + path + '. ' + str(ex))
    finally:
        setattr(cmds, busy_flag, False)
)PY";

    MStatus st = MGlobal::executePythonCommand(MString(kPythonScript));
    if (st != MS::kSuccess) {
        PluginLog::error("SafeOpen", "doIt: executePythonCommand failed");
        return st;
    }
    return MS::kSuccess;
}
