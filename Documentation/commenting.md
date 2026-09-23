# Comments and component documentation

The purpose of a comment is to explain a contract or a reason that the code
does not already make clear. Comments must describe the implementation that
exists, including its limitations, rather than the Linux behavior we might
implement later. Use English and normally keep lines within 80 columns.

## File overviews

Every first-party source file starts with an overview of its responsibility,
boundaries, and important invariants. Preserve SPDX lines and license or
copyright notices. A script keeps its shebang first; Makefiles and scripts
use their own comment syntax, not C block comments.

For example, the tree implementation has this kind of overview:

```c
/*
 * Intrusive red-black tree balancing and ordered traversal.
 * Callers choose keys and serialize mutations; the tree does not allocate
 * or own the objects containing its nodes.
 */
```

Do not add editor-generated author/date/path fields, slogans, an edit diary,
or an unrelated license. History belongs in Git. Retain attribution required
by existing source licenses even when removing obsolete editor metadata.

## Function contracts

Document every first-party kernel/framework function. Shared APIs and
nontrivial helpers use kernel-doc. A short ordinary comment is enough for a
trivial private helper; do not inflate a one-line predicate into a page of
boilerplate. A contract may live on the public declaration or the definition,
but should not be maintained twice.

```c
/**
 * rb_first() - Find the minimum linked node
 * @root: Initialized tree, possibly empty.
 *
 * Context: Caller prevents concurrent tree mutation; does not sleep.
 *
 * Return: Borrowed first node, or NULL for an empty tree.
 */
```

Use a standalone `/**`, a `name() - summary` line, and one `@argument:` entry
per parameter. `Context:` and `Return:` are separate sections, not text hidden
in an argument description. Void functions have no Return section.

The contract must answer the questions that matter to a caller:

- Which pointers may be NULL, and how large and accessible are their ranges?
- Are addresses virtual or physical; are lengths bytes, pages, or sectors?
- Which lock is required, which locks are taken, and can the function sleep?
- Is the result borrowed, newly referenced, consumed, or transferred?
- What is retained after failure or partial success, and who unwinds it?
- Which ordering, initialization, teardown, or interrupt constraints apply?
- Does failure return a negative Linux errno, an internal status, or panic?

Do not describe every negative result as `EINVAL`, promise thread safety
because a function takes one lock, or label a callback IRQ-safe without
following the entire callback path. For syscall wrappers whose C signature
is `void`, describe register argument decoding in prose rather than inventing
nonexistent `@argument` parameters.

## Implementation comments

Explain why a barrier, retry loop, lock handoff, rollback, or unusual ordering
is necessary. Put the explanation beside the operation it constrains. Keep
useful reasoning from old comments; remove commentary such as "increment i",
"initialize the lock", and obsolete commented-out implementations.

Assembly comments describe entry/exit registers, stack ownership, privilege
and address-space transitions, and clobbered state. Avoid translating each
instruction into English when a short block-level explanation is clearer.

Hardware-specific driver internals do not need kernel-doc on every routine.
They still need file overviews and explanations of register ordering, DMA
ownership, IRQ acknowledgement, queue lifetime, and failure cleanup. This
exception does not apply to reusable bus, device, IRQ, DMA, block, network,
TTY, UART, or VirtIO framework APIs.

Tests and build helpers need purpose and protocol comments where those are
not obvious. Document host versus guest execution, fixture prerequisites,
success markers, synchronization, and timeout assumptions. Do not add a full
kernel-API parameter template to every small test assertion.

## Component documents

Each feature component has a document under `Documentation/`, linked from
[the index](index.md). Cover its responsibilities, core objects, lifetimes,
normal and error flows, synchronization, external interfaces, known limits,
and reproducible tests. Explain the relationships, not a second copy of all
API comments. Update the document together with a behavior change.

Third-party libraries retain upstream comments and licenses. Document local
adapters and configuration; vendor modifications require a separate reason
and provenance update, not a blanket comment-style rewrite.

## Review and validation

Review comments against bodies and callers. Kernel-doc parsing finds syntax
and missing parameter descriptions, but cannot establish a locking contract
or prove that an error description is true. Some parsers silently skip
malformed one-line `/** name ... */` blocks, so check their structure too.

A documentation-only patch must preserve executable tokens and imported
source. Check C and assembly separately from scripts and Makefiles; treating
every file as C can hide a broken build-file comment. Run whitespace, build,
ABI, and runtime checks following [the validation guide](build-and-test.md).
Keep a pre-existing bug fix separate from the comment series, with its own
regression test; do not silently change behavior while explaining it.
