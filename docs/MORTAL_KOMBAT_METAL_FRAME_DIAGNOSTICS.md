# Mortal Kombat Metal frame diagnostics

This experimental branch is based on integration PR #292 and addresses issue
#293. It does not modify `MaxPS4-Beta` or a stable branch.

## Evidence and scope

The only game evidence at the start is a user observation on an iPhone 15 Pro
Max running iOS 27.2: Mortal Kombat: Legacy Kollection completes combat with
audio and controls, while observed frame rate varies around 55--60 FPS. Sonic
Mania was observed at 60 FPS. No device frame-time trace was supplied, so this
branch does not claim a bottleneck or an FPS improvement.

The Metal bridge already intentionally caps queued render command buffers at
two. When both are in flight, the guest submission thread waits for the oldest
one. That prevents unbounded latency, but can explain uneven frame delivery if
GPU completion is slow. It must be measured before changing the cap because a
larger queue may hide stutter while increasing input/display latency.

## Opt-in measurements

The developer-only **Metal frame-pacing metrics** switch is off by default. It
keeps at most 120 submissions and reports p50/p95/p99 for:

- total guest submission time;
- CPU encoding time;
- time blocked by the two-command queue cap; and
- completed command-buffer GPU execution time when Metal provides timestamps.

It does not use `waitUntilCompleted`, acquire a drawable, issue a readback, or
log once per frame. The existing draw-by-draw GPU program profiler remains a
separate, intrusive diagnostic and stays off during this trace.

## Device protocol and safe follow-up

On a cool device, enable only the frame-pacing switch, play a repeatable combat
segment for at least two minutes, then share the final MaxPS4 report with the
device model, iOS version, output resolution, Low Power Mode, and thermal
state. Repeat the same protocol for Sonic Mania. A p95/p99 dominated by queue
cap time indicates GPU pressure; a p95/p99 dominated by encoding points to
CPU/translation or pipeline work. Only then consider a narrowly tested change
to queue depth or pipeline scheduling, with Sonic Mania pixels and pacing kept
as a regression gate.
