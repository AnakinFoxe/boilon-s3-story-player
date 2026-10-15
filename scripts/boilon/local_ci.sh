#!/usr/bin/env bash
# Run the jobs of .github/workflows/build.yml on this Mac, in the workflow's own
# container image (Docker, e.g. OrbStack), each job on a clean clone of one
# commit, all jobs at once. Every `run` step runs as written, with its env and
# the matrix name, in the image's own environment as GitHub runs it; `uses`
# steps are skipped: the clean clone stands in for the checkout, and the apps
# are copied out instead of uploaded.
#
#   scripts/boilon/local_ci.sh [COMMIT]     (default HEAD; only committed files count)
#
# Prints each job's result, the unittest totals and both apps' size and SHA-256,
# and keeps the logs and apps in the directory it names. Exit status 0 means
# every job passed.
set -euo pipefail

repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
commit=$(git -C "$repo" rev-parse --verify "${1:-HEAD}^{commit}")
workflow=.github/workflows/build.yml
out=$(mktemp -d "${TMPDIR%/}/boilon-local-ci-${commit:0:8}.XXXXXX")
git -C "$repo" show "$commit:$workflow" >"$out/build.yml"
image=$(sed -n 's/^[[:space:]]*image:[[:space:]]*\([^[:space:]]*\).*/\1/p' "$out/build.yml" | sort -u)
if [ -z "$image" ] || [ "$(printf '%s\n' "$image" | wc -l)" -ne 1 ]; then
    echo "local_ci: expected exactly one container image in $workflow" >&2
    exit 2
fi
if [ -z "${1:-}" ] && [ -n "$(git -C "$repo" status --porcelain --untracked-files=no)" ]; then
    echo "local_ci: uncommitted changes are not tested, only $commit" >&2
fi

cat >"$out/runner.py" <<'PY'
"""Inside the container: list the workflow's jobs, or run one job's steps."""
import os
import subprocess
import sys

import yaml

JOB_KEYS = {"name", "needs", "strategy", "runs-on", "container", "steps"}
STEP_KEYS = {"name", "uses", "with", "run", "env", "shell"}
SHELLS = {None: ["bash", "-e", "-c"],
          "bash": ["bash", "--noprofile", "--norc", "-eo", "pipefail", "-c"]}


def fail(message):
    raise SystemExit("local_ci: " + message)


def jobs(workflow):
    for job_id, job in workflow["jobs"].items():
        if set(job) - JOB_KEYS:
            fail(f"job {job_id}: unsupported keys {sorted(set(job) - JOB_KEYS)}")
        matrix = job.get("strategy", {}).get("matrix", {})
        if set(matrix) - {"name"}:
            fail(f"job {job_id}: only a 'name' matrix is supported")
        for name in matrix.get("name", ["-"]):
            yield job_id, name


def run(workflow, job_id, name, base_env):
    for step in workflow["jobs"][job_id]["steps"]:
        title = step.get("name") or step.get("uses", "step")
        if set(step) - STEP_KEYS:
            fail(f"step {title}: unsupported keys {sorted(set(step) - STEP_KEYS)}")
        if "uses" in step:
            print(f"[local_ci] skip: {title} ({step['uses']})", flush=True)
            continue
        script = step["run"].replace("${{ matrix.name }}", name)
        env = {key: str(value) for key, value in step.get("env", {}).items()}
        if "${{" in script or any("${{" in value for value in env.values()):
            fail(f"step {title}: unsupported expression")
        if step.get("shell") not in SHELLS:
            fail(f"step {title}: unsupported shell {step.get('shell')}")
        print(f"[local_ci] run: {title}", flush=True)
        result = subprocess.run(SHELLS[step.get("shell")] + [script], env={**base_env, **env})
        if result.returncode:
            fail(f"step {title} exited {result.returncode}")


workflow = yaml.safe_load(open(sys.argv[2], encoding="utf-8"))
if sys.argv[1] == "list":
    for job_id, name in jobs(workflow):
        print(job_id, name)
else:
    # The steps get the image's environment, captured before export.sh, as on GitHub.
    pairs = open(sys.argv[5], "rb").read().split(b"\0")
    base = dict(pair.decode().split("=", 1) for pair in pairs if b"=" in pair)
    run(workflow, sys.argv[3], sys.argv[4], base)
PY

container() {
    docker run --rm --entrypoint bash -v "$out:/ci" "$@"
}
python_env='env -0 >/tmp/base.env && source "$IDF_PATH/export.sh" >/dev/null 2>&1 && exec python /ci/runner.py'
list=$(container "$image" -c "$python_env list /ci/build.yml")

echo "local_ci: commit $commit, image $image, output $out"
pids=()
names=()
while read -r job name; do
    tag=$job
    [ "$name" = - ] || tag=$name
    git clone --quiet --no-checkout "$repo" "$out/$tag"
    git -C "$out/$tag" checkout --quiet --detach "$commit"
    container -v "$out/$tag:/project" -w /project "$image" -c \
        "git config --global --add safe.directory '*' && $python_env run /project/$workflow $job $name /tmp/base.env" \
        >"$out/$tag.log" 2>&1 &
    pids+=("$!")
    names+=("$tag")
done <<<"$list"

status=0
for i in "${!pids[@]}"; do
    tag=${names[$i]}
    if wait "${pids[$i]}"; then result=passed; else result=FAILED; status=1; fi
    totals=$(grep -E '^(Ran [0-9]+ tests|OK|FAILED)' "$out/$tag.log" | tr '\n' ' ' || true)
    echo "local_ci: $tag $result; $totals"
    app=$out/$tag/build/xiaozhi.bin
    if [ "$result" = passed ] && [ -f "$app" ]; then
        cp "$app" "$out/$tag.bin"
        echo "local_ci:   $tag.bin $(stat -f %z "$app") bytes, SHA-256 $(shasum -a 256 "$app" | cut -d' ' -f1)"
    fi
    rm -rf "${out:?}/$tag"
done
echo "local_ci: logs and apps in $out"
exit "$status"
