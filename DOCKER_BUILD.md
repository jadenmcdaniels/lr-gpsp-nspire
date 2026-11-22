# Ndless Docker builds for `nspire-libretro-standalone`

This document describes the **`ndless-dev`** Docker Compose service used to build **`gpsp_libretro.tns`** without installing the Ndless SDK on the host.

## What it does (general)

- Runs a container that provides the **Ndless cross-toolchain** (`nspire-gcc`, `nspire-g++`, `genzehn`, and related tools) on Linux.
- **Mounts** the **parent** of `nspire-libretro-standalone` (the `gpsp-master` repo root) as **`/work`** inside the container.
- Sets the container **working directory** to **`/work/nspire-libretro-standalone`**, so `make` runs in the same layout as a local build from that folder.

The Compose file is [`docker-compose.yml`](./docker-compose.yml).

## Prerequisites

1. **Docker** with Compose v2 (`docker compose` subcommand).
2. A local image tagged **`ndless-dev:latest`** (default), **or** another image/tag via **`NSPIRE_BUILD_IMAGE`**.

Community references for building the image include **[stepney141/ndless-docker](https://github.com/stepney141/ndless-docker)**. After building or pulling, tag for Compose:

```bash
docker tag <your-image-id-or-name> ndless-dev:latest
```

Override without retagging:

```bash
export NSPIRE_BUILD_IMAGE=myregistry/ndless-sdk:2024   # bash
docker compose run --rm ndless-dev make
```

On **Windows PowerShell**: `$env:NSPIRE_BUILD_IMAGE = "myregistry/ndless-sdk:2024"` before `docker compose`, or a `.env` file beside `docker-compose.yml`.

## Typical commands

From **`nspire-libretro-standalone`** (same directory as `docker-compose.yml`):

```bash
docker compose run --rm ndless-dev make -j2
docker compose run --rm ndless-dev make clean all
docker compose run --rm ndless-dev sh
```

## Compose layout (summary)

| Item | Value |
|------|--------|
| Service name | `ndless-dev` |
| Image | `${NSPIRE_BUILD_IMAGE:-ndless-dev:latest}` |
| Volume | `..:/work` (repo root → `/work`) |
| Working dir | `/work/nspire-libretro-standalone` |

Paths like **`../libretro-gpsp`** in the Makefile resolve inside the container as **`/work/libretro-gpsp`**.

## Manual equivalent (without Compose)

```bash
docker run --rm \
  -v "/absolute/path/to/gpsp-master:/work" \
  -w /work/nspire-libretro-standalone \
  ndless-dev:latest \
  make -j2
```

## Ndless / genzehn metadata

The Makefile passes **`genzehn`** flags such as **`--ndless-min 31`** and **`--ndless-rev-min 2001`**. See **`README.txt`** and **`Makefile`** (`ZEHNFLAGS`) to change them.

## Troubleshooting

| Symptom | Things to check |
|--------|-------------------|
| `image not found` / pull errors | Image **`ndless-dev:latest`** exists, or set **`NSPIRE_BUILD_IMAGE`**. |
| Missing headers or `../libretro-gpsp` | Mount is the **repo root**; `gpsp-master` contains **`nspire-libretro-standalone`** and **`libretro-gpsp`**. |
| Stale objects after branch switch | **`docker compose run --rm ndless-dev make clean all`**. |
| Permission issues on Linux | The bind mount's user id. |

## Related docs

- **[README.txt](./README.txt)** — host build, ROM/bios layout, RAM flags, short Docker note.
