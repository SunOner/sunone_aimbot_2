# Data Collection

Configure the Collect Dataset node. For a new workspace, these config values seed its settings:

```ini
collect_data_while_playing = false
collect_only_when_targets_present = true
collect_save_every_n_frames = 15
collect_jpeg_quality = 95
auto_label_data = true
```

If capture diagnostics show `need_cpu_copy=true`, data collection may be one reason.

Related docs:

- [Capture diagnostics](capture-diagnostics.md)
- [Configuration guide](../config.md)
