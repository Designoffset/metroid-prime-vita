/* vita-elf-create appends its SCE data after the code segment: keep 8 KB free
   before the data segment so a build can never fail with "segment 1 overlaps". */
SECTIONS
{
  . = . + 0x2000;
}
INSERT BEFORE .init_array;
