pifx keeps its per-machine state here (all of it is ignored by git):

  settings.json   backend, outputs, input, master/output levels, every scope setting
  imgui.ini       window layout (delete it, or View > Reset layout, to get the default back)
  padmap.json     Launchpad layout, written with defaults on first run - edit freely
                  (actions are listed at the top of src/core/PadMap.h)
  presets/        saved sounds; slot1.json ... slot8.json are the Launchpad preset pads.
                  Presets from the old Python version load unchanged.
  hijack.json     only exists while system audio is hijacked; `pifx unhijack` uses it
                  to restore PipeWire routing after a crash
