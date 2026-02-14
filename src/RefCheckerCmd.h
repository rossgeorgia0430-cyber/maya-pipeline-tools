#pragma once
#ifndef REFCHECKERCMD_H
#define REFCHECKERCMD_H

#include <maya/MPxCommand.h>
#include <maya/MSyntax.h>
#include <maya/MArgList.h>

class RefCheckerCmd : public MPxCommand {
public:
    RefCheckerCmd();
    ~RefCheckerCmd() override;

    MStatus doIt(const MArgList& args) override;

    static void* creator();
    static MSyntax newSyntax();

    static const char* kCommandName;
};

#endif // REFCHECKERCMD_H
