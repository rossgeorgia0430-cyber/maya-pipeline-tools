#pragma once
#ifndef BATCHEXPORTERCMD_H
#define BATCHEXPORTERCMD_H

#include <maya/MPxCommand.h>
#include <maya/MSyntax.h>
#include <maya/MArgList.h>

class BatchExporterCmd : public MPxCommand {
public:
    BatchExporterCmd();
    ~BatchExporterCmd() override;

    MStatus doIt(const MArgList& args) override;

    static void* creator();
    static MSyntax newSyntax();

    static const char* kCommandName;
};

#endif // BATCHEXPORTERCMD_H
