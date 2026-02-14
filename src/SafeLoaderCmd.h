#pragma once
#ifndef SAFELOADERCMD_H
#define SAFELOADERCMD_H

#include <maya/MPxCommand.h>
#include <maya/MSyntax.h>
#include <maya/MArgList.h>

class SafeLoaderCmd : public MPxCommand {
public:
    SafeLoaderCmd();
    ~SafeLoaderCmd() override;

    MStatus doIt(const MArgList& args) override;

    static void* creator();
    static MSyntax newSyntax();

    static const char* kCommandName;
};

#endif // SAFELOADERCMD_H
