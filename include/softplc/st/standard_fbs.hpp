#pragma once

#include <string_view>

namespace softplc::st {

// Source text for the standard IEC 61131-3 timer/counter function blocks, written in
// the ST language itself (not native C++ intrinsics) and merged into every
// StProgram::load() call's POU registry -- see "Standard library: TON/TOF/CTU/CTD"
// in docs/architecture.md for the design rationale.
//
// TON/TOF accumulate elapsed time against "System.CycleTime", a well-known global
// TIME tag that ScanEngine writes every scan with the actual elapsed time since the
// previous scan started (see bindCompilationUnit() in pou_binder.cpp and
// ScanEngine::runOnce()). CTU/CTD use "R"/"LD" (not "RESET"), matching the real IEC
// standard's own parameter names and incidentally avoiding any collision with the
// RUNG statement's SET/RESET coil keywords.
//
// These names are reserved: a user CompilationUnit that defines its own
// FUNCTION_BLOCK/FUNCTION named TON/TOF/CTU/CTD fails to load with a "duplicate
// FUNCTION_BLOCK/FUNCTION name" error from bindCompilationUnit(), the same check
// used for any other duplicate POU name.
inline constexpr std::string_view kStandardFbLibrarySource = R"ST(
FUNCTION_BLOCK TON
VAR_INPUT
    IN : BOOL;
    PT : TIME;
END_VAR
VAR_OUTPUT
    Q : BOOL;
    ET : TIME;
END_VAR
IF IN THEN
    ET := ET + System.CycleTime;
    IF ET > PT THEN
        ET := PT;
    END_IF;
    IF ET >= PT THEN
        Q := TRUE;
    ELSE
        Q := FALSE;
    END_IF;
ELSE
    ET := T#0s;
    Q := FALSE;
END_IF;
END_FUNCTION_BLOCK

FUNCTION_BLOCK TOF
VAR_INPUT
    IN : BOOL;
    PT : TIME;
END_VAR
VAR_OUTPUT
    Q : BOOL;
    ET : TIME;
END_VAR
IF IN THEN
    Q := TRUE;
    ET := T#0s;
ELSE
    IF Q THEN
        ET := ET + System.CycleTime;
        IF ET > PT THEN
            ET := PT;
        END_IF;
        IF ET >= PT THEN
            Q := FALSE;
        END_IF;
    END_IF;
END_IF;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTU
VAR_INPUT
    CU : BOOL;
    R : BOOL;
    PV : DINT;
END_VAR
VAR_OUTPUT
    Q : BOOL;
    CV : DINT;
END_VAR
VAR
    M : BOOL;
END_VAR
IF R THEN
    CV := 0;
    M := FALSE;
ELSE
    IF CU AND NOT M THEN
        CV := CV + 1;
    END_IF;
    M := CU;
END_IF;
IF CV >= PV THEN
    Q := TRUE;
ELSE
    Q := FALSE;
END_IF;
END_FUNCTION_BLOCK

FUNCTION_BLOCK CTD
VAR_INPUT
    CD : BOOL;
    LD : BOOL;
    PV : DINT;
END_VAR
VAR_OUTPUT
    Q : BOOL;
    CV : DINT;
END_VAR
VAR
    M : BOOL;
END_VAR
IF LD THEN
    CV := PV;
    M := FALSE;
ELSE
    IF CD AND NOT M THEN
        IF CV > 0 THEN
            CV := CV - 1;
        END_IF;
    END_IF;
    M := CD;
END_IF;
IF CV <= 0 THEN
    Q := TRUE;
ELSE
    Q := FALSE;
END_IF;
END_FUNCTION_BLOCK
)ST";

}  // namespace softplc::st
