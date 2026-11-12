# FFS.so native engine migration

The exact Defragmenter 2026-10-31 AFFS native engine sources have been moved
into `native/defrag/` pending independent qualification. FFS-specific
format and allocation semantics belong here, not in Defragmenter. The unified
CMake driver adds a dedicated relocation and recovery interface
that uses only host-supplied format-neutral safety and raw I/O services.

The original freestanding FFS reader remains available unchanged through
`make`. The unified CMake target is a separate, currently unqualified candidate.
This commit does not yet migrate FFS Test Media formatter/populator, delete
the Defragmenter FFS engine, or claim crash/recovery qualification.
