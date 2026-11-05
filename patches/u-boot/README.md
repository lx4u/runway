# U-Boot patches

Patches for U-Boot (RFC v3), made with `git format-patch --cover-letter --subject-prefix="RFC PATCH v3"`. `0000-cover-letter.patch`
is the cover letter for the mailing list; it is not applied. They apply on upstream master `3401e006` ("qconfig: Improve the documentation").

They stop the EFI file protocol `Write()` from cutting a file at the end of the written range, which loses the rest
of `RUNWAY.DAT` on a FAT or exFAT ESP.

```sh
git -C u-boot am /path/to/runway/patches/u-boot/000[1-4]-*.patch
```

Drop a patch here when the U-Boot we build contains it.
