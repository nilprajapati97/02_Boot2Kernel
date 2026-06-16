# Module 9 — Writing a Camera Sensor Driver

> **Scope:** The complete anatomy of a Linux camera **sensor (I2C sub-device)** driver,
> built from scratch. Covers driver architecture, `probe()`/`remove()`, runtime PM and
> the power sequence, clock/regulator/GPIO/reset handling, I2C/CCI register access,
> the `v4l2_subdev` ops (`s_stream`, pad formats, controls), format/mode negotiation,
> control handling (exposure/gain/vblank/link-freq), async registration, and
> suspend/resume. This is where Modules 2, 3, 5, 6, 7, 8 converge into real code.

---

## 1. What a sensor driver actually is

A camera sensor driver is an **I2C client** that registers a **`v4l2_subdev`** into the
media graph. It:
- Acquires board resources (clock, regulators, reset GPIO) from DT (Module 7).
- Powers the sensor on/off in the correct sequence (Module 2).
- Talks to the sensor over I2C/CCI (Module 2/8) to read chip-ID and write mode tables.
- Exposes V4L2 controls (exposure, gain, vblank, link-freq) (Module 6).
- Advertises supported formats/modes as **pad formats** (Module 3/5).
- Starts/stops streaming via `s_stream` (Module 6/8).

```
 DT node ──► i2c_driver.probe() ──► v4l2_subdev (sensor entity)
                │                          │
        clk/regulator/gpio          media graph ──► CSIPHY ──► CSID ──► ISP
```

Reference drivers to study: `drivers/media/i2c/imx219.c`, `imx290.c`, `ov5640.c`,
`ov5675.c`. They are the canonical templates.

---

## 2. The driver skeleton

```c
struct sensor {
    struct v4l2_subdev sd;
    struct media_pad pad;
    struct i2c_client *client;
    struct regmap *regmap;                 /* register access */

    struct clk *xclk;                      /* master clock */
    u32 xclk_freq;
    struct gpio_desc *reset_gpio;
    struct regulator_bulk_data supplies[SENSOR_NUM_SUPPLIES];

    struct v4l2_ctrl_handler ctrls;
    struct v4l2_ctrl *exposure;
    struct v4l2_ctrl *gain;
    struct v4l2_ctrl *vblank;
    struct v4l2_ctrl *hblank;
    struct v4l2_ctrl *pixel_rate;
    struct v4l2_ctrl *link_freq;

    const struct sensor_mode *cur_mode;    /* current resolution/timing */
    struct mutex lock;                     /* serialize s_ctrl / s_stream */
    bool streaming;
};

#define to_sensor(_sd) container_of(_sd, struct sensor, sd)
```

---

## 3. Register access with regmap

`regmap_i2c` handles 16-bit register addresses + endianness cleanly:

```c
static const struct regmap_config sensor_regmap_cfg = {
    .reg_bits = 16,
    .val_bits = 8,
};

/* in probe: */
sensor->regmap = devm_regmap_init_i2c(client, &sensor_regmap_cfg);

/* helpers */
static int sensor_read(struct sensor *s, u16 reg, u8 *val) {
    unsigned int v;
    int ret = regmap_read(s->regmap, reg, &v);
    *val = v;
    return ret;
}
static int sensor_write(struct sensor *s, u16 reg, u8 val) {
    return regmap_write(s->regmap, reg, val);
}

/* write a whole mode register table */
static int sensor_write_regs(struct sensor *s, const struct reg_value *regs, int n) {
    for (int i = 0; i < n; i++) {
        int ret = sensor_write(s, regs[i].addr, regs[i].val);
        if (ret) return ret;
    }
    return 0;
}
```

On Qualcomm the underlying I2C transport is **CCI** (Module 2), but from the driver's
perspective it's still an I2C adapter; `regmap_i2c` works the same.

---

## 4. probe(): resource acquisition + identification

```c
static int sensor_probe(struct i2c_client *client)
{
    struct sensor *s;
    int ret;

    s = devm_kzalloc(&client->dev, sizeof(*s), GFP_KERNEL);
    s->client = client;

    /* 1. Parse the endpoint (lanes, link-freq, bus-type) — Module 7 */
    ret = sensor_parse_endpoint(s);

    /* 2. Master clock */
    s->xclk = devm_clk_get(&client->dev, "xclk");
    s->xclk_freq = clk_get_rate(s->xclk);
    if (s->xclk_freq != SENSOR_XCLK_FREQ)
        return dev_err_probe(&client->dev, -EINVAL, "bad xclk\n");

    /* 3. Regulators */
    for (int i = 0; i < SENSOR_NUM_SUPPLIES; i++)
        s->supplies[i].supply = sensor_supply_names[i];
    ret = devm_regulator_bulk_get(&client->dev, SENSOR_NUM_SUPPLIES, s->supplies);

    /* 4. Reset GPIO (asserted = in reset) */
    s->reset_gpio = devm_gpiod_get_optional(&client->dev, "reset", GPIOD_OUT_HIGH);

    /* 5. regmap */
    s->regmap = devm_regmap_init_i2c(client, &sensor_regmap_cfg);

    mutex_init(&s->lock);

    /* 6. Power on and verify chip-ID (the sanity gate) */
    ret = sensor_power_on(&client->dev);
    if (ret) return ret;
    ret = sensor_identify(s);              /* read chip-ID register */
    if (ret) goto err_power_off;

    /* 7. Init controls (exposure/gain/vblank/link-freq) */
    ret = sensor_init_controls(s);

    /* 8. Init the subdev + media pad */
    v4l2_i2c_subdev_init(&s->sd, client, &sensor_subdev_ops);
    s->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
    s->pad.flags = MEDIA_PAD_FL_SOURCE;
    s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
    ret = media_entity_pads_init(&s->sd.entity, 1, &s->pad);

    /* 9. Enable runtime PM (started powered-on above) */
    pm_runtime_set_active(&client->dev);
    pm_runtime_enable(&client->dev);
    pm_runtime_idle(&client->dev);

    /* 10. Register the subdev asynchronously into the graph — Module 6 */
    ret = v4l2_async_register_subdev_sensor(&s->sd);
    return 0;

err_power_off:
    sensor_power_off(&client->dev);
    return ret;
}
```

**Reading the chip-ID at probe is the single most important sanity gate** — if it fails,
power/clock/reset/I2C is wrong (Module 2 checklist).

---

## 5. The power sequence (runtime PM)

The power on/off is a strict, datasheet-defined sequence (Module 2 §8), implemented as
runtime-PM callbacks:

```c
static int sensor_power_on(struct device *dev)
{
    struct sensor *s = dev_to_sensor(dev);
    int ret;

    ret = regulator_bulk_enable(SENSOR_NUM_SUPPLIES, s->supplies);
    if (ret) return ret;

    ret = clk_prepare_enable(s->xclk);     /* start XCLK */
    if (ret) goto err_reg;

    gpiod_set_value_cansleep(s->reset_gpio, 0);  /* release reset (active-high desc) */
    usleep_range(1000, 1500);              /* t2: wait before first I2C */
    return 0;

err_reg:
    regulator_bulk_disable(SENSOR_NUM_SUPPLIES, s->supplies);
    return ret;
}

static int sensor_power_off(struct device *dev)
{
    struct sensor *s = dev_to_sensor(dev);
    gpiod_set_value_cansleep(s->reset_gpio, 1);  /* assert reset */
    clk_disable_unprepare(s->xclk);
    regulator_bulk_disable(SENSOR_NUM_SUPPLIES, s->supplies);
    return 0;
}

static const struct dev_pm_ops sensor_pm_ops = {
    SET_RUNTIME_PM_OPS(sensor_power_off, sensor_power_on, NULL)
};
```

Order and the `usleep_range` delays come straight from the datasheet. Wrong order/delay
→ no I2C ACK or garbage streaming (the #1 bring-up failure).

---

## 6. The subdev ops

```c
static const struct v4l2_subdev_video_ops sensor_video_ops = {
    .s_stream = sensor_s_stream,            /* start/stop streaming */
};

static const struct v4l2_subdev_pad_ops sensor_pad_ops = {
    .enum_mbus_code   = sensor_enum_mbus_code,
    .enum_frame_size  = sensor_enum_frame_size,
    .get_fmt          = sensor_get_fmt,
    .set_fmt          = sensor_set_fmt,
    .get_selection    = sensor_get_selection,   /* crop bounds */
};

static const struct v4l2_subdev_core_ops sensor_core_ops = {
    .subscribe_event   = v4l2_ctrl_subdev_subscribe_event,
    .unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops sensor_subdev_ops = {
    .core  = &sensor_core_ops,
    .video = &sensor_video_ops,
    .pad   = &sensor_pad_ops,
};
```

---

## 7. Modes and format negotiation

A sensor supports a table of modes (Module 3 §9):

```c
struct sensor_mode {
    u32 width, height;
    u32 hts;                 /* line_length_pck */
    u32 vts_def, vts_min;    /* frame_length_lines */
    u32 code;                /* media bus code, e.g. MEDIA_BUS_FMT_SRGGB10_1X10 */
    const struct reg_value *regs;
    unsigned int num_regs;
};

static const struct sensor_mode sensor_modes[] = {
    { 1920, 1080, 0x0d78, 0x0459, 0x0440, MEDIA_BUS_FMT_SRGGB10_1X10,
      mode_1080p_regs, ARRAY_SIZE(mode_1080p_regs) },
    { 1280,  720, 0x0d78, 0x02ee, 0x02d0, MEDIA_BUS_FMT_SRGGB10_1X10,
      mode_720p_regs,  ARRAY_SIZE(mode_720p_regs) },
};
```

`set_fmt` picks the closest mode and updates control ranges that depend on it:

```c
static int sensor_set_fmt(struct v4l2_subdev *sd,
                          struct v4l2_subdev_state *state,
                          struct v4l2_subdev_format *fmt)
{
    struct sensor *s = to_sensor(sd);
    const struct sensor_mode *mode;

    mode = v4l2_find_nearest_size(sensor_modes, ARRAY_SIZE(sensor_modes),
                                  width, height,
                                  fmt->format.width, fmt->format.height);
    fmt->format.width  = mode->width;
    fmt->format.height = mode->height;
    fmt->format.code   = mode->code;

    if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
        *v4l2_subdev_state_get_format(state, 0) = fmt->format;
        return 0;
    }

    mutex_lock(&s->lock);
    s->cur_mode = mode;
    /* update exposure/vblank ranges for the new mode's timing */
    sensor_update_control_ranges(s, mode);
    mutex_unlock(&s->lock);
    return 0;
}
```

`enum_mbus_code` / `enum_frame_size` let userspace (and the ISP) discover what the
sensor offers — this is how `media-ctl -p` shows available formats.

---

## 8. Control handling (the AE knobs)

```c
static int sensor_init_controls(struct sensor *s)
{
    struct v4l2_ctrl_handler *hdl = &s->ctrls;
    const struct sensor_mode *m = s->cur_mode;

    v4l2_ctrl_handler_init(hdl, 10);

    /* exposure (in lines); max depends on VTS */
    s->exposure = v4l2_ctrl_new_std(hdl, &sensor_ctrl_ops,
                    V4L2_CID_EXPOSURE, 1, m->vts_def - 4, 1, m->vts_def - 8);

    s->gain = v4l2_ctrl_new_std(hdl, &sensor_ctrl_ops,
                    V4L2_CID_ANALOGUE_GAIN, 0, 978, 1, 0);

    /* vblank controls fps; changing it re-ranges exposure */
    s->vblank = v4l2_ctrl_new_std(hdl, &sensor_ctrl_ops,
                    V4L2_CID_VBLANK, m->vts_min - m->height,
                    0xffff - m->height, 1, m->vts_def - m->height);

    s->hblank = v4l2_ctrl_new_std(hdl, &sensor_ctrl_ops,
                    V4L2_CID_HBLANK, m->hts - m->width, m->hts - m->width,
                    1, m->hts - m->width);
    if (s->hblank) s->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

    /* link freq (read-only menu) + pixel rate — consumed by CSIPHY */
    s->link_freq = v4l2_ctrl_new_int_menu(hdl, &sensor_ctrl_ops,
                    V4L2_CID_LINK_FREQ, ARRAY_SIZE(link_freqs)-1, 0, link_freqs);
    if (s->link_freq) s->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

    s->pixel_rate = v4l2_ctrl_new_std(hdl, &sensor_ctrl_ops,
                    V4L2_CID_PIXEL_RATE, PIXEL_RATE, PIXEL_RATE, 1, PIXEL_RATE);

    s->sd.ctrl_handler = hdl;
    return hdl->error;
}

static int sensor_s_ctrl(struct v4l2_ctrl *ctrl)
{
    struct sensor *s = ctrl_to_sensor(ctrl);
    int ret;

    /* When VBLANK changes, exposure max must follow (= VTS - margin) */
    if (ctrl->id == V4L2_CID_VBLANK) {
        int exp_max = s->cur_mode->height + ctrl->val - 4;
        __v4l2_ctrl_modify_range(s->exposure,
                                 s->exposure->minimum, exp_max,
                                 1, s->exposure->default_value);
    }

    /* Only touch hardware if the sensor is powered (streaming) */
    if (pm_runtime_get_if_in_use(&s->client->dev) == 0)
        return 0;

    switch (ctrl->id) {
    case V4L2_CID_EXPOSURE:
        ret = sensor_write_exposure(s, ctrl->val);     /* COARSE_INTEG regs */
        break;
    case V4L2_CID_ANALOGUE_GAIN:
        ret = sensor_write_gain(s, ctrl->val);          /* ANA_GAIN regs */
        break;
    case V4L2_CID_VBLANK:
        ret = sensor_write_vts(s, s->cur_mode->height + ctrl->val);
        break;
    default:
        ret = -EINVAL;
    }

    pm_runtime_put(&s->client->dev);
    return ret;
}

static const struct v4l2_ctrl_ops sensor_ctrl_ops = { .s_ctrl = sensor_s_ctrl };
```

The `pm_runtime_get_if_in_use()` guard is essential: controls can be set while the
sensor is powered off (userspace caches values); only write registers when powered.

---

## 9. s_stream: start/stop streaming

```c
static int sensor_s_stream(struct v4l2_subdev *sd, int enable)
{
    struct sensor *s = to_sensor(sd);
    int ret;

    mutex_lock(&s->lock);
    if (enable) {
        ret = pm_runtime_resume_and_get(&s->client->dev);   /* power on */
        if (ret < 0) goto unlock;

        /* 1. write the current mode's register table */
        ret = sensor_write_regs(s, s->cur_mode->regs, s->cur_mode->num_regs);
        if (ret) goto err_rpm_put;

        /* 2. apply cached controls (exposure/gain/vblank) to HW */
        ret = __v4l2_ctrl_handler_setup(&s->ctrls);
        if (ret) goto err_rpm_put;

        /* 3. start streaming (write MODE_SELECT = streaming) */
        ret = sensor_write(s, REG_MODE_SELECT, MODE_STREAMING);
        if (ret) goto err_rpm_put;

        s->streaming = true;
    } else {
        sensor_write(s, REG_MODE_SELECT, MODE_STANDBY);     /* stop */
        s->streaming = false;
        pm_runtime_mark_last_busy(&s->client->dev);
        pm_runtime_put_autosuspend(&s->client->dev);        /* allow power off */
    }
    mutex_unlock(&s->lock);
    return 0;

err_rpm_put:
    pm_runtime_put(&s->client->dev);
unlock:
    mutex_unlock(&s->lock);
    return ret;
}
```

The CSI/ISP bridge calls this `s_stream` down the chain at STREAMON (Module 5/6). Order
matters: write mode table → apply controls → set streaming bit. Setting the streaming
bit before the mode table = garbage frames.

---

## 10. Streaming call flow (end to end)

```
 userspace: VIDIOC_STREAMON on /dev/video0
   │
 capture driver start_streaming() (vb2)
   │  video_device_pipeline_start()  → validate graph (Module 5)
   ▼
 v4l2_subdev_call(ife,  video, s_stream, 1)    ISP: configure & start
 v4l2_subdev_call(csid, video, s_stream, 1)    CSID: program VC/DT, settle
 v4l2_subdev_call(csiphy,video, s_stream, 1)   CSIPHY: lanes + HS settle from link_freq
 v4l2_subdev_call(sensor,video, s_stream, 1)   SENSOR: write mode regs, set streaming
   │
   ▼
 sensor emits FS/lines/FE → CSIPHY → CSID (CRC/ECC ok) → ISP → DMA → DDR
   │
   ▼
 vb2 buffer DONE → userspace DQBUF
```

The CSIPHY reads `V4L2_CID_LINK_FREQ` from the sensor's control handler (via the media
graph) to compute settle timing — the link between Module 8 and this driver.

---

## 11. remove() and suspend/resume

```c
static void sensor_remove(struct i2c_client *client)
{
    struct sensor *s = i2c_get_clientdata(client);
    v4l2_async_unregister_subdev(&s->sd);
    v4l2_subdev_cleanup(&s->sd);
    media_entity_cleanup(&s->sd.entity);
    v4l2_ctrl_handler_free(&s->ctrls);
    pm_runtime_disable(&client->dev);
    if (!pm_runtime_status_suspended(&client->dev))
        sensor_power_off(&client->dev);
    pm_runtime_set_suspended(&client->dev);
    mutex_destroy(&s->lock);
}

/* System suspend: if streaming, stop and power off; resume restarts. */
static int __maybe_unused sensor_suspend(struct device *dev)
{
    struct sensor *s = dev_to_sensor(dev);
    if (s->streaming) sensor_stop_streaming(s);
    return 0;
}
static int __maybe_unused sensor_resume(struct device *dev)
{
    struct sensor *s = dev_to_sensor(dev);
    if (s->streaming) return sensor_start_streaming(s);
    return 0;
}
```

Runtime PM (per-use power on/off) and system PM (suspend-to-RAM) are distinct; both must
be handled or you leak power or lose state across suspend.

---

## 12. Driver registration boilerplate

```c
static const struct of_device_id sensor_of_match[] = {
    { .compatible = "sony,imx219" },
    { }
};
MODULE_DEVICE_TABLE(of, sensor_of_match);

static struct i2c_driver sensor_i2c_driver = {
    .driver = {
        .name = "imx219",
        .of_match_table = sensor_of_match,
        .pm = &sensor_pm_ops,
    },
    .probe  = sensor_probe,
    .remove = sensor_remove,
};
module_i2c_driver(sensor_i2c_driver);
```

---

## 13. Debugging a sensor driver

```bash
# Probe-time
dmesg | grep -iE "imx219|chip id|probe|EPROBE_DEFER"   # chip-ID ok?
cat /sys/kernel/debug/devices_deferred                 # stuck deferring?

# Controls
v4l2-ctl -d /dev/v4l-subdev0 --list-ctrls              # ranges sane?
v4l2-ctl -d /dev/v4l-subdev0 --set-ctrl exposure=2000

# Formats
media-ctl -p -d /dev/media0                            # advertised modes/bus codes

# Streaming
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=30

# Register-level (dynamic debug on the driver)
echo 'module imx219 +p' > /sys/kernel/debug/dynamic_debug/control
```

Symptom → cause:
```
 Chip-ID read fails at probe   → power seq/order, XCLK absent, reset polarity, I2C addr
 Probe never completes (defer) → missing clock/regulator/remote-endpoint (DT)
 Controls have wrong ranges    → mode timing (HTS/VTS) math wrong
 Image dark / no exposure resp → s_ctrl not writing, or units (lines vs us) wrong
 Garbled frames at stream start → streaming bit set before mode table written
 Works then dies after suspend → suspend/resume not restarting streaming
 CRC errors at CSID            → link_freq control wrong → CSIPHY settle wrong (Mod 8)
```

---

## 14. Interview Q&A

**Q1. What is the single most important check in a sensor driver's probe()?**
Reading the chip-ID register after power-on. It validates that power sequencing, clock,
reset polarity, and I2C addressing are all correct before you commit to registering the
subdev. If it fails, the bring-up problem is in those basics.

**Q2. Why guard `s_ctrl` with `pm_runtime_get_if_in_use()`?**
Userspace can set controls while the sensor is powered off; their values are cached in
the control framework. You must only write registers when the sensor is actually
powered. `pm_runtime_get_if_in_use()` returns 0 (skip the HW write) if it's suspended,
avoiding I2C access to a dead device.

**Q3. Walk the order of operations inside `s_stream(1)`.**
Power on (runtime PM resume) → write the current mode's register table → apply cached
controls via `__v4l2_ctrl_handler_setup` → set the streaming/MODE_SELECT register.
Order matters: setting the streaming bit before the mode table produces garbage frames.

**Q4. How does changing VBLANK affect exposure, and how do you handle it?**
VBLANK sets VTS (frame length), and exposure must be ≤ VTS − margin. In `s_ctrl`, when
VBLANK changes you recompute the exposure max and call `__v4l2_ctrl_modify_range()` on
the exposure control so userspace sees the new valid range.

**Q5. How does the CSIPHY learn the link frequency from the sensor?**
The sensor exposes a read-only `V4L2_CID_LINK_FREQ` int-menu control. The CSIPHY/bridge
driver reads it through the media graph (via the connected subdev's control handler) and
computes D-PHY HS settle timing from it. A wrong link-freq → wrong settle → CRC errors.

**Q6. Difference between runtime PM and system suspend in a sensor driver?**
Runtime PM powers the sensor on/off per use (e.g. between streaming sessions) via
`SET_RUNTIME_PM_OPS`. System suspend (suspend-to-RAM) must stop streaming and power off
on `.suspend`, then restart streaming on `.resume` if it was active. They are separate
callback sets and both must be implemented.

**Q7. How are sensor modes represented and how does `set_fmt` use them?**
As a table of `{width, height, hts, vts, bus_code, register_table}`. `set_fmt` picks the
nearest mode to the requested size (`v4l2_find_nearest_size`), updates the reported
format/bus code, and re-ranges timing-dependent controls (exposure/vblank). The register
table is written at `s_stream`.

**Q8. Why use regmap_i2c instead of raw i2c_transfer?**
regmap abstracts register width/endianness, provides read/write/bulk helpers, optional
caching and debugfs register dumps, and reduces boilerplate. For a 16-bit-addr/8-bit-val
sensor it's a few lines vs hand-rolled message structs.

**Q9. A sensor streams fine at 720p but shows CRC errors at 1080p. Where do you look?**
Higher resolution = higher link rate = tighter D-PHY timing. Check that the 1080p mode
advertises the correct (higher) link frequency and that the CSIPHY settle timing is
recomputed for it. Often the mode table's PLL/link-freq or the advertised
`V4L2_CID_LINK_FREQ` is wrong for the high-res mode (Module 8).

---

### Key takeaways
- A sensor driver is an I2C client exposing a `v4l2_subdev`; probe acquires
  clk/regulator/gpio, powers on, and verifies chip-ID before registering.
- The power sequence (order + delays) lives in runtime-PM callbacks; it's the top
  bring-up failure point.
- Controls (exposure in lines, analog gain, vblank, link-freq, pixel-rate) drive AE and
  CSI timing; guard HW writes with runtime-PM and re-range exposure when vblank changes.
- `s_stream`: power on → mode table → apply controls → streaming bit; the bridge calls it
  down the chain, and the CSIPHY reads link-freq for settle timing.
