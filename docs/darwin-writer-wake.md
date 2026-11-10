# Darwin writer wake

The Call of Duty device report showed multiple threads waiting in
`os_sync_wait_on_address`, including threads returning from HLE after cancellation.
The report does not identify the exact mutex or prove this is the only cause.

The pinned Darwin FEX writer-priority mutex has a concrete lost-progress case:
readers and writers wait on the same address, but the writer notification wakes
only one arbitrary waiter. If that waiter is a reader, writer priority makes it
sleep again. An eligible writer can remain asleep with no lock owner left to
provide another notification. Linux uses filtered notifications and is unaffected.

The dependency patch wakes all waiters for writer notifications on Darwin.
Acquisition still checks the existing atomic predicates; exclusivity and writer
priority are unchanged. This can increase wakeups under contention.

Validation includes a deterministic notification regression using the actual
pinned method (old wake fails; replacement succeeds), patch idempotence and
source-drift rejection, and a bounded macOS test of the real pinned mutex with
four writers and eight readers (40000 writes, 80000 reads). The build applies
the same patch to macOS and iPhone dependencies. Device replay remains to be
verified with the generated IPA.
