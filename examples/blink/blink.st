(* Toggles %Q0.0 every ToggleAfterScans scan cycles.
   At the default 10ms cycle time, 50 scans is roughly 500ms, i.e. a 1Hz blink. *)
PROGRAM Blink
VAR
    Counter : DINT := 0;
    ToggleAfterScans : DINT := 50;
    LedState AT %Q0.0 : BOOL := FALSE;
END_VAR

Counter := Counter + 1;
IF Counter >= ToggleAfterScans THEN
    LedState := NOT LedState;
    Counter := 0;
END_IF;
END_PROGRAM
