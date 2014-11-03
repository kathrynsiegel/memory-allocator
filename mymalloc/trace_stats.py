import math

bucket_sizes = [2**p for p in range(5, 30)]

def fit_into_bucket(val):
    for bucket in bucket_sizes:
        if val + 8 <= bucket:
            return bucket

for i in range(10):
    with open("traces/trace_c" + str(i) + "_v0", "r") as trace:
        mysterynum = trace.readline()
        num_ops = trace.readline()
        num_allocs= trace.readline()
        something = trace.readline()
        allocs = {}
        real_allocs = {}

        totalmem = 0
        maxmem = 0
        realmem = 0
        maxrealmem = 0
        for line in trace:
            data = line.split()
            if data[0] == 'a':
                allocs[int(data[1])] = fit_into_bucket(int(data[2]))
                real_allocs[int(data[1])] = int(data[2])

                realmem += int(data[2])
                totalmem += allocs[int(data[1])]
                maxmem = max(totalmem, maxmem)
                maxrealmem = max(realmem, maxrealmem)

            elif data[0] == 'f':
                totalmem -= allocs[int(data[1])]
                realmem -= real_allocs[int(data[1])]

        print
        print i
        print "max memory usage:", maxmem
        print "minimum possible memory usage:", maxrealmem
        print "ratio", float(maxrealmem)/maxmem
        print sorted({v: allocs.values().count(v) for v in allocs.values()}.items())
