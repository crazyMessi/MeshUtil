# General V2 Parallel Collapse Design

## Scope

General V2 keeps the existing QEM edge-collapse framework. The validated S mesh
has one connected component containing 100% of its 8,972,290 vertices and
17,944,222 faces, so component-level parallelism is not useful.

## Write-set conflict radius

A collapse modifies more than its two endpoints:

1. radial faces incident to the collapsed edge are removed;
2. the removed vertex disk is spliced into the kept vertex disk;
3. duplicate edges around both endpoints are merged;
4. costs are updated for the kept vertex one-ring;
5. costs are also updated for outer edges of the kept-vertex face fan.

Two candidates that merely have distinct endpoints can therefore still write the
same disk/radial links or heap entries. The first implementation treats the
closed two-hop vertex neighborhoods of both endpoints as the conflict set.

## General V2 staging

1. Pop a bounded top-K window from the global cost heap.
2. Validate candidate IDs and capture current endpoints/costs.
3. Greedily select a deterministic two-hop independent set.
4. Reinsert candidates rejected only by conflict.
5. Evaluate target, topology, and flip checks in parallel using thread-local
   scratch and read-only mesh state.
6. Commit accepted collapses serially in captured cost/order sequence.
7. Before each commit, revalidate edge liveness/endpoints; skip stale candidates
   and let local heap updates determine their future state.

This staging changes global collapse order and therefore is a General V2 quality
candidate, not a strict-output path. The serial commit phase is retained until a
stronger proof and locking scheme exists for concurrent disk/radial mutation.

## Metrics

Report candidate window size, selected independent-set size, stale count,
conflict count, topology/flip rejection counts, parallel evaluation wall,
serial commit wall, and total collapse wall for 1/2/4/8/16/32 threads.

## Rejected prototype: global heap independent-set batches

The conservative two-hop prototype was implemented and passed tiny cases plus
formal topology smoke tests. It used thread-local read scratch, a persistent
worker group, deterministic serial commits, stale revalidation, and no lost
heap candidates.

It was rejected for performance:

- serial baseline: about 75 seconds on the 17.9-million-face S case;
- two threads, 128-candidate window: more than 180 seconds;
- two threads, 4096-candidate window: 122.6 seconds;
- four threads, 256-candidate window: about 108.5 seconds.

Parallel evaluation itself dropped to 5-10 seconds, but repeatedly extracting a
global top-K window, building two-hop conflict sets, and refreshing conflicting
candidates dominated runtime. Increasing the window did not solve the problem.

General V2 must therefore avoid repeated global-heap round trips. The next
parallel design should use persistent spatial or graph partitions with local
candidate queues, protect partition boundaries during local collapse, then run
a global cleanup pass.

## Selected design: persistent partition-local QEM

The selected next design does not split or duplicate submeshes. It keeps the
existing stable vertex, edge, face, and loop IDs, and assigns ownership over the
existing topology:

1. Stable-sort alive vertices by a 63-bit Morton code, with `VertexId` as the
   tie-break.
2. Assign partition owners by the prefix sum of active face-corner weights,
   rather than by uniform spatial cell width.
3. Mark cross-partition edge endpoints, then expand a protected halo by two
   graph rings.
4. Place only same-owner, non-halo edges in persistent partition-local heaps.
5. Run the original serial QEM collapse loop inside each partition while
   different partitions execute concurrently.
6. Use fixed per-partition face-removal quotas and barrier-level repartitioning.
7. Rebuild a global heap once at the end and run the existing serial cleanup to
   the exact target.

Partition count is fixed by the reduction stage, not by worker count, so the
same partition schedule should produce the same output at 2/4/8/16/32 workers.
The initial S-mesh schedule to evaluate is:

| Final target | Partition schedule | Last local-stage target |
| ---: | --- | ---: |
| 3M | 128 | 3.2M |
| 1M | 128 → 64 | 1.10M |
| 0.3M | 128 → 64 → 16 | 0.36M |

Artificial partition boundaries must not receive boundary quadrics. They are
scheduling constraints, not geometric boundaries.

### Partition ownership

- Vertex owner remains fixed within an epoch.
- An edge is local only when both endpoint owners match.
- A face is local-core only when all three owners match and no vertex is in the
  protected halo.
- A candidate must have both endpoints and their closed two-hop neighborhoods
  inside the same unprotected core.

This guarantees disjoint disk/radial/face/heap write sets between simultaneously
running partitions without per-edge locks.

### Local targets and cleanup

For an epoch with current face count `F`, local-stage target `G`, and local-core
face counts `L_p`, allocate removal quotas using largest remainder:

`R_p = floor((F - G) * L_p / sum(L_p))`.

Ties use partition ID. Workers never steal quota during an epoch. Unfilled
one-face gaps and deferred boundary edges are handled by the final global
cleanup. Cleanup and repartition barriers are the only global coordination
points.

### Memory constraints

The implementation must reuse one global edge-entry storage and one
`EdgeId → position` array across all local heaps. It must not allocate:

- a complete topology stamp array per worker;
- a complete heap-position array per partition;
- copied vertex/face/loop arrays per partition;
- physically cut submeshes that later require welding.

For the 17.9M-face S mesh, the planned owner/flag, reject bitmap, Morton keys,
IDs, and radix-sort buffers should keep the partitioning transient below roughly
0.3 GiB.

## Dry-run gate

Before changing collapse behavior, implement a partition dry-run that reports
for `P = 16/32/64/128`:

- partition face-corner load min/mean/max and max/mean ratio;
- cross-partition edge count and fraction;
- protected halo vertex and face count/fraction;
- locally eligible edge count/fraction after the dynamic two-hop rule;
- estimated local heap entries per partition;
- Morton partitioning wall time and transient bytes.

The dry-run must leave the output and serial SHA unchanged. A partition count is
eligible for the local-collapse MVP only when load max/mean is at most 1.10 and
the protected halo does not remove most candidate edges.
