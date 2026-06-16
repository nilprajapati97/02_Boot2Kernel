# Module 5 — The Linux Media Framework (Media Controller)

> **Scope:** The Media Controller (MC) framework — the kernel's model for representing
> a complex camera pipeline as a graph of entities, pads, and links. Covers why V4L2
> alone wasn't enough, the data structures (`media_device`, `media_entity`,
> `media_pad`, `media_link`), sub-devices vs video nodes, pipeline validation, graph
> traversal, and the `media-ctl` userspace workflow. This is the backbone that ties
> sensor + CSI + ISP drivers together.

---

## 1. Why the Media Controller exists

Classic V4L2 (Module 6) modeled a camera as a single `/dev/video0` doing everything.
That breaks for modern SoC cameras where the pipeline is many independent IP blocks
with configurable routing:

```
 sensor → CSIPHY → CSID → IFE/ISP → (multiple outputs) → DMA → video nodes
```

Problems V4L2-only couldn't express:
- **Multiple components** each needing independent configuration (formats, crop).
- **Configurable routing** (which sensor → which CSI → which ISP output).
- **Format propagation/validation** across the chain before streaming.
- **Discoverability** — userspace needs to *see* the topology.

The **Media Controller** solves this by exposing the pipeline as a **directed graph**
through `/dev/mediaN`, with each block a node userspace can inspect and connect.

```
 V4L2 alone:                    Media Controller:
 ┌──────────────┐               ┌────────┐  ┌──────┐  ┌──────┐  ┌─────┐
 │ /dev/video0  │               │ sensor │─►│ csid │─►│ ife  │─►│video│
 │  (monolith)  │               └────────┘  └──────┘  └──────┘  └─────┘
 └──────────────┘                 subdev      subdev    subdev    node
                                  each independently configurable
```

---

## 2. Core concepts: entity, pad, link, graph

```
 ┌───────────────┐         link          ┌───────────────┐
 │   ENTITY A    │  pad0 ──────────────► │   ENTITY B    │
 │  (sensor)     │ (source)       (sink) │  (csid)       │
 └───────────────┘                       └───────┬───────┘
                                         pad0 src │
                                                  ▼
                                          ┌───────────────┐
                                          │   ENTITY C    │
                                          │  (ife/isp)    │
                                          └───────────────┘
```

- **Entity** — a functional block (sensor, CSI receiver, ISP, video DMA node, lens).
- **Pad** — a connection point on an entity. **Source pad** = output, **sink pad** =
  input. An entity can have many pads.
- **Link** — a connection between a source pad and a sink pad. Links can be
  **enabled/disabled** (route selection) and **immutable** (fixed wiring).
- **Graph** — the whole set of entities + pads + links, rooted at a `media_device`.

This mirrors the **physical/logical dataflow** of the hardware exactly.

---

## 3. Kernel data structures

Header: `include/media/media-device.h`, `media-entity.h`.

```c
struct media_device {              /* one per pipeline, exposes /dev/mediaN */
    struct device *dev;
    struct media_devnode *devnode;
    struct list_head entities;     /* all entities */
    struct list_head pads;
    struct list_head links;
    const struct media_device_ops *ops;
    ...
};

struct media_entity {              /* a block in the graph */
    struct media_gobj graph_obj;
    const char *name;              /* e.g. "msm_csid0" */
    enum media_entity_type type;
    u32 function;                  /* MEDIA_ENT_F_CAM_SENSOR, _VID_IF_BRIDGE, ... */
    u16 num_pads;
    struct media_pad *pads;        /* array of pads */
    struct list_head links;
    const struct media_entity_operations *ops;  /* .link_setup, .link_validate */
};

struct media_pad {
    struct media_gobj graph_obj;
    struct media_entity *entity;   /* owner */
    u16 index;                     /* pad number */
    unsigned long flags;           /* MEDIA_PAD_FL_SINK | _SOURCE */
};

struct media_link {
    struct media_pad *source;
    struct media_pad *sink;
    unsigned long flags;           /* MEDIA_LNK_FL_ENABLED | _IMMUTABLE | _DYNAMIC */
};
```

### Registration flow (what a driver does)
```c
media_device_init(&cam->mdev);
media_entity_pads_init(&subdev->entity, num_pads, pads);   /* declare pads */
v4l2_device_register_subdev(...);                          /* register subdev */
media_create_pad_link(src_ent, src_pad, sink_ent, sink_pad, flags); /* wire */
media_device_register(&cam->mdev);                         /* expose /dev/mediaN */
```

---

## 4. Sub-devices vs video nodes — the crucial distinction

```
 SUB-DEVICE (v4l2_subdev)             VIDEO NODE (video_device)
 ────────────────────────             ─────────────────────────
 /dev/v4l-subdevX                     /dev/videoX
 An internal processing block         The DMA endpoint userspace streams from
 (sensor, CSI, ISP block)             (where buffers are queued/dequeued)
 Configured via subdev ioctls:        Configured via V4L2 ioctls:
   set_fmt on each PAD                  VIDIOC_S_FMT (whole device)
   set_selection (crop/compose)         VIDIOC_REQBUFS / QBUF / DQBUF
   pad-level media bus codes            VIDIOC_STREAMON
 No buffers of its own                 Owns the videobuf2 queue
```

Mental model:
- **Sub-devices** are the *internal* nodes of the graph — you set **pad formats** on
  them to describe the data flowing through.
- **Video nodes** are the *leaves* (DMA sinks/sources) — you queue **buffers** there.

A pipeline typically has several `/dev/v4l-subdevN` (one per sensor/CSI/ISP block) and
one or more `/dev/videoN` (the capture DMA endpoints).

---

## 5. Format propagation and pipeline validation

Before streaming, formats must be **consistent along the whole graph**: the source
pad format of one entity must match the sink pad format of the next.

```
 sensor:src  [SRGGB10 1920x1080]
                  │  must match
 csid:sink   [SRGGB10 1920x1080]
 csid:src    [SRGGB10 1920x1080]
                  │  must match
 ife:sink    [SRGGB10 1920x1080]
 ife:src     [NV12    1920x1080]   (ISP converts)
                  │  must match
 video:      [NV12    1920x1080]
```

- Userspace sets pad formats with `VIDIOC_SUBDEV_S_FMT` on each subdev pad (via
  `media-ctl --set-v4l2`).
- At `STREAMON`, the kernel walks the pipeline and each entity's
  `.link_validate()` checks its sink format matches the upstream source format. A
  mismatch fails streaming with `-EPIPE` — a *very* common bring-up error.

```c
/* Each subdev implements this; called during media_pipeline_start() */
static int my_link_validate(struct v4l2_subdev *sd,
                            struct media_link *link,
                            struct v4l2_subdev_format *source_fmt,
                            struct v4l2_subdev_format *sink_fmt)
{
    if (source_fmt->format.width  != sink_fmt->format.width  ||
        source_fmt->format.height != sink_fmt->format.height ||
        source_fmt->format.code   != sink_fmt->format.code)
        return -EPIPE;
    return 0;
}
```

---

## 6. Pipeline start/stop and the pipeline object

When userspace calls `STREAMON` on the video node, the V4L2 core (or the driver)
starts the whole **pipeline**:

```c
/* drivers call this to lock & validate the graph for streaming */
ret = video_device_pipeline_start(vdev, &pipe);
/* internally: builds the connected graph, calls link_validate on every link,
   sets media_pipeline on each pad to prevent topology changes while streaming */
```

Key rules:
- While a pipeline is streaming, its **links are locked** — you can't re-route.
- `media_pipeline_stop()` releases them on `STREAMOFF`.
- The graph walk uses `media_graph_walk_*` to traverse all reachable entities.

---

## 7. Graph traversal APIs

```c
struct media_graph graph;
media_graph_walk_init(&graph, mdev);
media_graph_walk_start(&graph, entity);
while ((entity = media_graph_walk_next(&graph))) {
    /* visit every entity reachable from the start entity */
}
media_graph_walk_cleanup(&graph);

/* find what's connected to a pad: */
struct media_pad *remote = media_pad_remote_pad_first(pad);
struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(remote->entity);
```

Drivers use these to find the upstream sensor from the ISP, propagate
controls/formats, and start sub-devices in order.

---

## 8. Userspace workflow with media-ctl

`media-ctl` is *the* tool for inspecting and configuring the graph.

```bash
# Print the whole topology (entities, pads, links, current formats)
media-ctl -p -d /dev/media0

# Example output (abridged):
# - entity 1: imx219 4-0010 (1 pad, 1 link)
#       pad0: Source  [fmt:SRGGB10/1920x1080]
#       -> "msm_csid0":0 [ENABLED]
# - entity 5: msm_vfe0_video (1 pad, 1 link)
#       pad0: Sink  [fmt:NV12/1920x1080]

# Enable a link (route sensor -> csid)
media-ctl -d /dev/media0 -l '"imx219 4-0010":0 -> "msm_csid0":0[1]'

# Set a pad format (propagate along the chain)
media-ctl -d /dev/media0 -V '"imx219 4-0010":0 [fmt:SRGGB10/1920x1080]'
media-ctl -d /dev/media0 -V '"msm_csid0":1   [fmt:SRGGB10/1920x1080]'

# Then stream from the resulting video node
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
         --stream-mmap --stream-count=100
```

The standard bring-up loop: `media-ctl -p` to see topology → enable links → set pad
formats end-to-end → `v4l2-ctl` to stream. If `STREAMON` fails with `-EPIPE`, a pad
format along the chain doesn't match.

---

## 9. The MEDIA_IOC ioctls (UAPI)

Header: `include/uapi/linux/media.h`.

```
MEDIA_IOC_DEVICE_INFO        get media device info
MEDIA_IOC_G_TOPOLOGY         get full topology (entities/pads/links) in one call
MEDIA_IOC_ENUM_ENTITIES      (legacy) enumerate entities
MEDIA_IOC_ENUM_LINKS         enumerate a entity's pads & links
MEDIA_IOC_SETUP_LINK         enable/disable a link
MEDIA_IOC_REQUEST_ALLOC      allocate a Request (Request API, Module 6)
```

`media-ctl` and `libcamera` are built on these. `MEDIA_IOC_G_TOPOLOGY` is the modern
one-shot way to read the entire graph.

---

## 10. How a real pipeline maps (Qualcomm CAMSS example)

```
 /dev/media0
 ┌──────────┐  ┌────────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────┐
 │ imx_sensor│─►│ msm_csiphyN │─►│ msm_csidN │─►│ msm_vfeN │─►│ msm_vfeN_video│
 │  subdev  │  │   subdev    │  │  subdev   │  │  subdev  │  │  video node   │
 └──────────┘  └────────────┘  └──────────┘  └──────────┘  └──────────────┘
   sensor        CSI D-PHY        CSI decode    ISP/IFE       DMA → DDR (vb2)
   (i2c)                          VC/DT demux   processing     /dev/videoN
```

Each box is a `media_entity`; the arrows are `media_link`s you enable with `media-ctl
-l`. The Qualcomm CAMSS driver (`drivers/media/platform/qcom/camss`) creates all these
entities and links at probe (Module 11).

---

## 11. Debugging the media graph

```bash
media-ctl -p -d /dev/media0          # full topology + current formats
media-ctl --print-dot -d /dev/media0 | dot -Tpng -o graph.png   # visualize!

# Common failures:
#  STREAMON returns -EPIPE  → pad formats mismatch along chain (fix with -V)
#  No link enabled          → media-ctl -l to route (sensor->csid->ife->video)
#  Wrong video node size    → set_fmt on video must match ISP src pad
```

The `--print-dot` → Graphviz trick gives you a visual pipeline diagram — invaluable on
a complex multi-camera SoC.

---

## 12. Interview Q&A

**Q1. Why did the Media Controller framework get added when V4L2 already existed?**
V4L2 modeled a camera as one monolithic device. Modern SoC pipelines are many
independently-configurable IP blocks with selectable routing. MC exposes the pipeline
as a discoverable graph of entities/pads/links so userspace can inspect topology,
choose routes, and propagate/validate formats across the whole chain.

**Q2. Difference between a sub-device and a video node?**
A sub-device (`/dev/v4l-subdevN`) is an internal processing block configured via
pad-level formats/selection; it owns no buffers. A video node (`/dev/videoN`) is the
DMA endpoint that owns the videobuf2 queue where userspace QBUF/DQBUFs frames.

**Q3. What is a source pad vs a sink pad?**
A source pad outputs data (`MEDIA_PAD_FL_SOURCE`); a sink pad receives data
(`MEDIA_PAD_FL_SINK`). A link always connects a source pad to a sink pad.

**Q4. STREAMON fails with -EPIPE. What does that tell you?**
Pipeline validation failed: the format on some sink pad doesn't match the upstream
source pad (resolution or media bus code mismatch). Fix by setting consistent pad
formats end-to-end with `media-ctl -V`.

**Q5. How does the kernel prevent topology changes during streaming?**
`media_pipeline_start()` marks every pad in the active pipeline with a
`media_pipeline` pointer, locking links so `SETUP_LINK` can't re-route while streaming;
`media_pipeline_stop()` releases it on STREAMOFF.

**Q6. How does an ISP driver find the sensor connected upstream?**
By walking the graph: follow the sink pad's enabled link to the remote source pad
(`media_pad_remote_pad_first`), convert the remote entity to a `v4l2_subdev`
(`media_entity_to_v4l2_subdev`), repeating until it reaches the sensor entity.

**Q7. What does `media-ctl -p` show and when do you use it?**
It prints the full topology: entities, their pads, current pad formats, and link
states (enabled/disabled). It's the first command in any camera bring-up to understand
routing and find format mismatches.

**Q8. What's the role of `.link_validate()`?**
It's the per-subdev callback invoked during pipeline start for each link; it checks the
sink format matches the connected source format (width/height/code) and returns -EPIPE
on mismatch, preventing a misconfigured pipeline from streaming.

---

### Key takeaways
- The Media Controller models the pipeline as a graph: entities (blocks), pads
  (source/sink connection points), links (connections), under `/dev/mediaN`.
- Sub-devices carry pad formats (internal nodes); video nodes carry buffers (leaves).
- Formats must propagate and validate along the chain; mismatch → `-EPIPE` at STREAMON.
- `media-ctl -p` / `--print-dot` and `media-ctl -l/-V` are your topology inspect/route
  tools; this graph is what ties sensor, CSI, and ISP drivers together (Modules 9–12).
