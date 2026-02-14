#include "BatchExporterCmd.h"
#include "BatchExporterUI.h"

const char* BatchExporterCmd::kCommandName = "batchAnimExporter";

BatchExporterCmd::BatchExporterCmd() {}
BatchExporterCmd::~BatchExporterCmd() {}

void* BatchExporterCmd::creator() {
    return new BatchExporterCmd();
}

MSyntax BatchExporterCmd::newSyntax() {
    MSyntax syntax;
    return syntax;
}

MStatus BatchExporterCmd::doIt(const MArgList& /*args*/) {
    // Create and show the dialog
    BatchExporterUI::showUI();

    return MS::kSuccess;
}
