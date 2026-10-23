# Development Log



---



ran, and tested it for real.


failed on one line:

```

```



coreutils first, on the host:

```

```



### On-device E2E — PS3 / RPCS3


crashes, see below):

```
# → nasdl.self
```


window sends keys to the GUI and nothing happens).


Netdebug output (`stderr` → RPCS3 `sys_tty_write`):

```

```

Server access log for the same request:

```
[harness] /Homebrew/ResumeTest.bin Range=bytes=14941184-
```

On-disk file: `16777216 bytes`, SHA-256



share that strips `Range` and answers `200`:

```
download start ... resume_offset=3145728
response status=200 mode=200 restart resume_offset=0
download complete bytes=16777216 sha256=9361...8874
```


instead of appending to a partial.


digest is still auditable.

### Still open



  file. Fine for a test, noisy for a real log.
  
  download self-verifying.

### Regression check



---


console output.


both succeed now.



every request is **HTTP/1.0 200 OK**:

- downloaded file **SHA-256 matches** the source.


end-to-end, not just in the unit test.)


resume path is an open decision.

---



`.self`.** Two things had to line up:


   *is* observable/testable on RPCS3 today.


  `/dev_hdd0/NASDL/`, **SHA-256 matches source**.

Two bugs surfaced on the PS3 side (not yet fixed):

  a clean segment layout too.

how they were found.

---



### Build verification (real toolchains)


```bash

```

  `nasdl.self` (real CEX SELF) and `nasdl.pkg`.

in the working tree — **not yet committed**):

  `ppu-gcc` to override make's built-in `ld`).
  
  `padData` has no `BTN` field / `PAD_BTN_*` macros.

### On-device E2E — PSP / PPSSPP ✅


   `psp/main.c` and `ps3/main.c`.


**crashes immediately at C-startup**:

```
```

the loaded ELF:


rather than a silent assumption.

### Status / next steps

  resume, navigation) after the two fixes above.
