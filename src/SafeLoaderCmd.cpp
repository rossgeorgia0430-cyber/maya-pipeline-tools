#include "SafeLoaderCmd.h"
#include "SafeLoaderUI.h"

#include <maya/MGlobal.h>
#include <maya/MQtUtil.h>

const char* SafeLoaderCmd::kCommandName = "safeLoadRefs";

SafeLoaderCmd::SafeLoaderCmd() {}
SafeLoaderCmd::~SafeLoaderCmd() {}

void* SafeLoaderCmd::creator() {
    return new SafeLoaderCmd();
}

MSyntax SafeLoaderCmd::newSyntax() {
    MSyntax syntax;
    return syntax;
}

MStatus SafeLoaderCmd::doIt(const MArgList& /*args*/) {
    SafeLoaderUI::showUI();
    return MS::kSuccess;
}
