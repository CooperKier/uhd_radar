#!/usr/bin/env bash
# Run on the new Ubuntu ARM64 Pi, from a fresh repository checkout.
set -euo pipefail
bundle_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [ "$(uname -m)" != aarch64 ]; then
    echo 'This bootstrap targets a 64-bit ARM Raspberry Pi (aarch64).' >&2
    exit 1
fi
cd "$bundle_dir"
sudo apt-get update
sudo apt-get install -y ca-certificates curl git build-essential pkg-config usbutils unzip

conda_bin=''
if command -v conda >/dev/null 2>&1; then
    conda_bin=$(command -v conda)
else
    for candidate in "$HOME/miniforge3/bin/conda" "$HOME/miniconda3/bin/conda" "$HOME/miniconda/bin/conda"; do
        if [ -x "$candidate" ]; then conda_bin="$candidate"; break; fi
    done
fi
if [ -z "$conda_bin" ]; then
    installer_dir=$(mktemp -d)
    trap 'rm -rf -- "$installer_dir"' EXIT
    installer="$installer_dir/Miniforge3-26.7.2-0-Linux-aarch64.sh"
    curl -fL --retry 2 https://github.com/conda-forge/miniforge/releases/download/26.7.2-0/Miniforge3-26.7.2-0-Linux-aarch64.sh -o "$installer"
    printf '%s  %s\n' 89b786c8d2c8b0fda7553914c1314ae4ddaa094503802f279377b19ac4463cb2 "$installer" | sha256sum --check -
    bash "$installer" -b -p "$HOME/miniforge3"
    conda_bin="$HOME/miniforge3/bin/conda"
fi
conda_base=$("$conda_bin" info --base)
if [ -d "$conda_base/envs/uhd-b206/conda-meta" ]; then
    echo 'Existing uhd-b206 environment preserved; verify its versions using the setup guide.'
else
    "$conda_bin" env create --solver libmamba -f "$bundle_dir/environment-b206.yaml"
fi
"$conda_bin" run --no-capture-output -n uhd-b206 uhd_config_info --version
printf '\nIn your current terminal, activate with:\nsource "%s/etc/profile.d/conda.sh"\nconda activate uhd-b206\n' "$conda_base"
