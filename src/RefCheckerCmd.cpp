#include "RefCheckerCmd.h"
#include "RefCheckerUI.h"

const char* RefCheckerCmd::kCommandName = "refChecker";

RefCheckerCmd::RefCheckerCmd() {}
RefCheckerCmd::~RefCheckerCmd() {}

void* RefCheckerCmd::creator() {
    return new RefCheckerCmd();
}

MSyntax RefCheckerCmd::newSyntax() {
    MSyntax syntax;
    return syntax;
}

MStatus RefCheckerCmd::doIt(const MArgList& /*args*/) {
    // Create and show the dialog
    RefCheckerUI::showUI();

    return MS::kSuccess;
}
