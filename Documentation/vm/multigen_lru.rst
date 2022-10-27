.. SPDX-License-Identifier: GPL-2.0

=============
Multi-Gen LRU
=============
The multi-gen LRU is an alternative LRU implementation that optimizes
page reclaim and improves performance under memory pressure. Page
reclaim decides the kernel's caching policy and ability to overcommit
memory. It directly impacts the kswapd CPU usage and RAM efficiency.

Design overview
===============
Objectives
----------
The design objectives are:

* Good representation of access recency
* Try to profit from spatial locality
* Fast paths to make obvious choices
* Simple self-correcting heuristics

The representation of access recency is at the core of all LRU
implementations. In the multi-gen LRU, each generation represents a
group of pages with similar access recency. Generations establish a
(time-based) common frame of reference and therefore help make better
choices, e.g., between different memcgs on a computer or different
computers in a data center (for job scheduling).

Exploiting spatial locality improves efficiency when gathering the
accessed bit. A rmap walk targets a single page and does not try to
profit from discovering a young PTE. A page table walk can sweep all
the young PTEs in an address space, but the address space can be too
sparse to make a profit. The key is to optimize both methods and use
them in combination.

Fast paths reduce code complexity and runtime overhead. Unmapped pages
do not require TLB flushes; clean pages do not require writeback.
These facts are only helpful when other conditions, e.g., access
recency, are similar. With generations as a common frame of reference,
additional factors stand out. But obvious choices might not be good
choices; thus self-correction is necessary.

The benefits of simple self-correcting heuristics are self-evident.
Again, with generations as a common frame of reference, this becomes
attainable. Specifically, pages in the same generation can be
categorized based on additional factors, and a feedback loop can
statistically compare the refault percentages across those categories
and infer which of them are better choices.

Assumptions
-----------
The protection of hot pages and the selection of cold pages are based
on page access channels and patterns. There are two access channels:

* Accesses through page tables
* Accesses through file descriptors

The protection of the former channel is by design stronger because:

1. The uncertainty in determining the access patterns of the former
   channel is higher due to the approximation of the accessed bit.
2. The cost of evicting the former channel is higher due to the TLB
   flushes required and the likelihood of encountering the dirty bit.
3. The penalty of underprotecting the former channel is higher because
   applications usually do not prepare themselves for major page
   faults like they do for blocked I/O. E.g., GUI applications
   commonly use dedicated I/O threads to avoid blocking rendering
   threads.

There are also two access patterns:

* Accesses exhibiting temporal locality
* Accesses not exhibiting temporal locality

For the reasons listed above, the former channel is assumed to follow
the former pattern unless ``VM_SEQ_READ`` or ``VM_RAND_READ`` is
present, and the latter channel is assumed to follow the latter
pattern unless outlying refaults have been observed.

Workflow overview
=================
Evictable pages are divided into multiple generations for each
``lruvec``. The youngest generation number is stored in
``lrugen->max_seq`` for both anon and file types as they are aged on
an equal footing. The oldest generation numbers are stored in
``lrugen->min_seq[]`` separately for anon and file types as clean file
pages can be evicted regardless of swap constraints. These three
variables are monotonically increasing.

Generation numbers are truncated into ``order_base_2(MAX_NR_GENS+1)``
bits in order to fit into the gen counter in ``page->flags``. Each
truncated generation number is an index to ``lrugen->lists[]``. The
sliding window technique is used to track at least ``MIN_NR_GENS`` and
at most ``MAX_NR_GENS`` generations. The gen counter stores a value
within ``[1, MAX_NR_GENS]`` while a page is on one of
``lrugen->lists[]``; otherwise it stores zero.

Each generation is divided into multiple tiers. A page accessed ``N``
times through file descriptors is in tier ``order_base_2(N)``. Unlike
generations, tiers do not have dedicated ``lrugen->lists[]``. In
contrast to moving across generations, which requires the LRU lock,
moving across tiers only involves atomic operations on
``page->flags`` and therefore has a negligible cost. A feedback loop
modeled after the PID controller monitors refaults over all the tiers
from anon and file types and decides which tiers from which types to
evict or protect.

There are two conceptually independent procedures: the aging and the
eviction. They form a closed-loop system, i.e., the page reclaim.

Aging
-----
The aging produces young generations. Given an ``lruvec``, it
increments ``max_seq`` when ``max_seq-min_seq+1`` approaches
``MIN_NR_GENS``. The aging promotes hot pages to the youngest
generation when it finds them accessed through page tables; the
demotion of cold pages happens consequently when it increments
``max_seq``. The aging uses page table walks and rmap walks to find
young PTEs. For the former, it iterates ``lruvec_memcg()->mm_list``
and calls ``walk_page_range()`` with each ``mm_struct`` on this list
to scan PTEs, and after each iteration, it increments ``max_seq``. For
the latter, when the eviction walks the rmap and finds a young PTE,
the aging scans the adjacent PTEs. For both, on finding a young PTE,
the aging clears the accessed bit and updates the gen counter of the
page mapped by this PTE to ``(max_seq%MAX_NR_GENS)+1``.

Eviction
--------
The eviction consumes old generations. Given an ``lruvec``, it
increments ``min_seq`` when ``lrugen->lists[]`` indexed by
``min_seq%MAX_NR_GENS`` becomes empty. To select a type and a tier to
evict from, it first compares ``min_seq[]`` to select the older type.
If both types are equally old, it selects the one whose first tier has
a lower refault percentage. The first tier contains single-use
unmapped clean pages, which are the best bet. The eviction sorts a
page according to its gen counter if the aging has found this page
accessed through page tables and updated its gen counter. It also
moves a page to the next generation, i.e., ``min_seq+1``, if this page
was accessed multiple times through file descriptors and the feedback
loop has detected outlying refaults from the tier this page is in. To
this end, the feedback loop uses the first tier as the baseline, for
the reason stated earlier.

Summary
-------
The multi-gen LRU can be disassembled into the following parts:

* Generations
* Rmap walks
* Page table walks
* Bloom filters
* PID controller

The aging and the eviction form a producer-consumer model;
specifically, the latter drives the former by the sliding window over
generations. Within the aging, rmap walks drive page table walks by
inserting hot densely populated page tables to the Bloom filters.
Within the eviction, the PID controller uses refaults as the feedback
to select types to evict and tiers to protect.


Quick start
===========
Build the kernel with the following configurations.

* ``CONFIG_LRU_GEN=y``
* ``CONFIG_LRU_GEN_ENABLED=y``

All set!

Runtime options
===============
``/sys/kernel/mm/lru_gen/`` contains stable ABIs described in the
following subsections.

Kill switch
-----------
``enabled`` accepts different values to enable or disable the
following components. Its default value depends on
``CONFIG_LRU_GEN_ENABLED``. All the components should be enabled
unless some of them have unforeseen side effects. Writing to
``enabled`` has no effect when a component is not supported by the
hardware, and valid values will be accepted even when the main switch
is off.

====== ===============================================================
Values Components
====== ===============================================================
0x0001 The main switch for the multi-gen LRU.
0x0002 Clearing the accessed bit in leaf page table entries in large
       batches, when MMU sets it (e.g., on x86). This behavior can
       theoretically worsen lock contention (mmap_lock). If it is
       disabled, the multi-gen LRU will suffer a minor performance
       degradation for workloads that contiguously map hot pages,
       whose accessed bits can be otherwise cleared by fewer larger
       batches.
0x0004 Clearing the accessed bit in non-leaf page table entries as
       well, when MMU sets it (e.g., on x86). This behavior was not
       verified on x86 varieties other than Intel and AMD. If it is
       disabled, the multi-gen LRU will suffer a negligible
       performance degradation.
[yYnN] Apply to all the components above.
====== ===============================================================

E.g.,
::

    echo y >/sys/kernel/mm/lru_gen/enabled
    cat /sys/kernel/mm/lru_gen/enabled
    0x0007
    echo 5 >/sys/kernel/mm/lru_gen/enabled
    cat /sys/kernel/mm/lru_gen/enabled
    0x0005

Thrashing prevention
--------------------
Personal computers are more sensitive to thrashing because it can
cause janks (lags when rendering UI) and negatively impact user
experience. The multi-gen LRU offers thrashing prevention to the
majority of laptop and desktop users who do not have ``oomd``.

Users can write ``N`` to ``min_ttl_ms`` to prevent the working set of
``N`` milliseconds from getting evicted. The OOM killer is triggered
if this working set cannot be kept in memory. In other words, this
option works as an adjustable pressure relief valve, and when open, it
terminates applications that are hopefully not being used.

Based on the average human detectable lag (~100ms), ``N=1000`` usually
eliminates intolerable janks due to thrashing. Larger values like
``N=3000`` make janks less noticeable at the risk of premature OOM
kills.

The default value ``0`` means disabled.

Experimental features
=====================
``/sys/kernel/debug/lru_gen`` accepts commands described in the
following subsections. Multiple command lines are supported, so does
concatenation with delimiters ``,`` and ``;``.

``/sys/kernel/debug/lru_gen_full`` provides additional stats for
debugging. ``CONFIG_LRU_GEN_STATS=y`` keeps historical stats from
evicted generations in this file.

Working set estimation
----------------------
Working set estimation measures how much memory an application needs
in a given time interval, and it is usually done with little impact on
the performance of the application. E.g., data centers want to
optimize job scheduling (bin packing) to improve memory utilizations.
When a new job comes in, the job scheduler needs to find out whether
each server it manages can allocate a certain amount of memory for
this new job before it can pick a candidate. To do so, the job
scheduler needs to estimate the working sets of the existing jobs.

When it is read, ``lru_gen`` returns a histogram of numbers of pages
accessed over different time intervals for each memcg and node.
``MAX_NR_GENS`` decides the number of bins for each histogram. The
histograms are noncumulative.
::

    memcg  memcg_id  memcg_path
       node  node_id
           min_gen_nr  age_in_ms  nr_anon_pages  nr_file_pages
           ...
           max_gen_nr  age_in_ms  nr_anon_pages  nr_file_pages

Each bin contains an estimated number of pages that have been accessed
within ``age_in_ms``. E.g., ``min_gen_nr`` contains the coldest pages
and ``max_gen_nr`` contains the hottest pages, since ``age_in_ms`` of
the former is the largest and that of the latter is the smallest.

Users can write the following command to ``lru_gen`` to create a new
generation ``max_gen_nr+1``:

    ``+ memcg_id node_id max_gen_nr [can_swap [force_scan]]``

``can_swap`` defaults to the swap setting and, if it is set to ``1``,
it forces the scan of anon pages when swap is off, and vice versa.
``force_scan`` defaults to ``1`` and, if it is set to ``0``, it
employs heuristics to reduce the overhead, which is likely to reduce
the coverage as well.

A typical use case is that a job scheduler runs this command at a
certain time interval to create new generations, and it ranks the
servers it manages based on the sizes of their cold pages defined by
this time interval.

Proactive reclaim
-----------------
Proactive reclaim induces page reclaim when there is no memory
pressure. It usually targets cold pages only. E.g., when a new job
comes in, the job scheduler wants to proactively reclaim cold pages on
the server it selected, to improve the chance of successfully landing
this new job.

Users can write the following command to ``lru_gen`` to evict
generations less than or equal to ``min_gen_nr``.

    ``- memcg_id node_id min_gen_nr [swappiness [nr_to_reclaim]]``

``min_gen_nr`` should be less than ``max_gen_nr-1``, since
``max_gen_nr`` and ``max_gen_nr-1`` are not fully aged (equivalent to
the active list) and therefore cannot be evicted. ``swappiness``
overrides the default value in ``/proc/sys/vm/swappiness``.
``nr_to_reclaim`` limits the number of pages to evict.

A typical use case is that a job scheduler runs this command before it
tries to land a new job on a server. If it fails to materialize enough
cold pages because of the overestimation, it retries on the next
server according to the ranking result obtained from the working set
estimation step. This less forceful approach limits the impacts on the
existing jobs.
