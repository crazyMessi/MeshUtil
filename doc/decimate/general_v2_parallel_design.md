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
