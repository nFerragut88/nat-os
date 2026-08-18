# used_medias

Fiction about this operating system, written alongside it.

Not documentation. The numbered UM-NATOS reports are the record of what is
actually true; nothing here constrains the kernel. It is kept in the repository
because UM-NATOS-029 §2 cites it, and a report that quotes a source the
repository does not contain has a dead reference in it.

## What is here

**`natOS.odt`** — *NAT OS: An Experience Inside the Machine.* Forty chapters,
roughly a hundred thousand words. A system reconstructing its own history from
partial records, in which the subsystems are characters and the records are the
plot. Chapter Fourteen ends with `CHAPTER ONE BEGINS AGAIN.`

Its subject is not computers. It is continuity — how something can change
without becoming unrelated to what came before, remember without inventing, and
interpret without rewriting the evidence. The closing loop states it directly:

```c
while (system_exists) {
    observe();
    remember();
    interpret();
    justify();
    act();
    record();
    revise();
    succeed();
    do_not_falsify_history();
}
```

That last line is the engineering discipline of the whole project written as a
function call, and the reports were keeping it before the novel named it: dead
theories recorded so nobody re-tests them, a reverted fix left reverted rather
than described as working, a self-test that prints `ZERO REJECTS, the test is
inert` because a check that cannot fail is worse than no check.

**`um_1.txt` and the chapter files** — *The NatOS Organization: A Novel of
System Architecture.* The same premise in a different register: the subsystems
as office staff, the Scheduler as a nervous man with priority lists. Lighter,
funnier, and the source the book's voice came from.

Its first chapter contains the line that started a real investigation — the
Scheduler refusing Touch a context switch because *"the 3D raycast renderer is
in the middle of a DMA transfer."* Every clause of that excuse turned out to be
a live issue, and one of them was false in a way that mattered. See UM-NATOS-029
§2 and UM-NATOS-030.
