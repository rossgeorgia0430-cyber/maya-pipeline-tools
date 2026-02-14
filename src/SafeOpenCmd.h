#pragma once
#ifndef SAFEOPENCMD_H
#define SAFEOPENCMD_H

#include <maya/MPxCommand.h>
#include <maya/MSyntax.h>
#include <maya/MArgList.h>

class SafeOpenCmd : public MPxCommand {
public:
    SafeOpenCmd();
    ~SafeOpenCmd() override;

    MStatus doIt(const MArgList& args) override;

    static void* creator();
    static MSyntax newSyntax();

    static const char* kCommandName;
};

#endif // SAFEOPENCMD_H
