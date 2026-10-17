import resource, subprocess, sys
subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
r = resource.getrusage(resource.RUSAGE_CHILDREN)
print("%.3f" % (r.ru_utime + r.ru_stime))
