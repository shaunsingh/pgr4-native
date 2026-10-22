#!/usr/bin/env python3
"""Samples the APU every few ms for correlating frame-time spikes with clock or power drops
(CLOCK_MONOTONIC_RAW ms, which Wine's QueryPerformanceCounter follows): the SMU's average GFX
clock, socket/CPU/GFX power, memory and fabric clocks and power-limit throttle bits (gpu_metrics
v2.2-v2.4), and GPU busy %.
  gpu_clock_sampler.py <csv> <seconds> [interval_ms=10]"""
import glob, os, struct, sys, time
out, secs = sys.argv[1], float(sys.argv[2])
iv = float(sys.argv[3]) / 1000 if len(sys.argv) > 3 else 0.01
dev = next(d for d in sorted(glob.glob('/sys/class/drm/card[0-9]/device')) if os.path.exists(d + '/gpu_busy_percent'))
metrics = open(dev + '/gpu_metrics', 'rb')
busy = open(dev + '/gpu_busy_percent')
head = metrics.read(4)
if head[2] != 2 or head[3] < 2:
    sys.exit(f'gpu_metrics v{head[2]}.{head[3]}: expected an APU layout v2.2-v2.4')
end = time.monotonic() + secs
with open(out, 'w') as o:
    o.write('t_ms,sclk_mhz,power_w,cpu_w,gfx_w,busy,mclk_mhz,fclk_mhz,power_throttle\n')
    while time.monotonic() < end:
        t = time.clock_gettime(time.CLOCK_MONOTONIC_RAW) * 1000
        metrics.seek(0); b = metrics.read()
        busy.seek(0); load = int(busy.read())
        socket, cpu, _soc, gfx = struct.unpack_from('<4H', b, 40)
        gfxclk, _socclk, uclk, fclk = struct.unpack_from('<4H', b, 64)
        throttle = struct.unpack_from('<Q', b, 120)[0] & 0xFFFF
        o.write(f'{t:.3f},{gfxclk},{socket / 1000:.2f},{cpu / 1000:.2f},{gfx / 1000:.2f},{load},{uclk},{fclk},{throttle}\n')
        time.sleep(iv)
