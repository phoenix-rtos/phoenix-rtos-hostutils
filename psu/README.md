# psu

SDP script syntax:

- WAIT `<vid>` `<pid>`

  Description: Wait on HID device.

- WRITE\_FILE `<F/S>` `<quoted string>` `[address]` `[format]` `[offset]` `[size]`
  
  Description: Host sends file or data to HID device.
  Note: use `format=0` (default) for typical file transfer (skips bad blocks). `format=1` is used for direct write at specified `address` (if `address` points to a bad block the operation completes without error despite file not being transfered), e.g. used for writing multiple control block or partition table copies.
  Example:

  ```
  WRITE_FILE F "phoenix-kernel.img" 0xac000000 0 0 16384 # addr=0xac000000 format=0 off=0 size=16384
  ```

  ```
  WRITE_FILE F "phoenix-kernel.img" 0xac000000 0 16384   # addr=0xac000000 format=0 off=16384 size=<whole file>
  ```

  ```
  WRITE_FILE S "\x00\x00\x00\x80\xff\xff\xff\x87\x00\x00\x00\x00\x00\x80\x3a\x49\x00\x00\x00\x00 Xpsd;/dev/flash0;/dev/flash1;/" 0x80000020 0 0 # addr=0x80000020 format=0 off=0 size=<whole file>
  ```
  
- WRITE\_REGISTER `<address>` `<value>` `<format 8/16/32>`
  
  Description: Host sends WRITE\_REGISTER command to device, to write value to the adress specified in the ADRESS field. PSD uses this command to handle action.
  
  Addresses use in PSD:
  
  |COMMAND | ADDRESS | VALUE |
  | --- | --- | --- |
  |CHANGE\_PARTITION|-1|file number|
  |ERASE\_PARTITION\_ADDRESS |-2 |size to erase since the beginning of partition (or 0 for full partition). Address format: 0-> just erase, 8-> erase and write jffs2 cleanmarkers, 16-> just write jffs2 cleanmarkers|
  |~~ERASE\_CHIP\_ADDRESS~~ |-3|**obsolete, use ERASE_PARTITION_ADDRESS with flashX device** ~~size to erase since the beginning of chip (or 0 for full chip)~~|
  |~~CHECK\_PRODUCTION~~ |-4|**obsolete** ~~none~~|
  |CONTROL\_BLOCK\_ADDRESS|-5|1 - FCB; 2 - DBBT|
  |BLOW\_FUSES|-6|none|
  |CLOSE\_PSD|-100|0|
  
  Example:

  ```
  WRITE_REGISTER -1 0 8 # switches file to file 0
  ```

  ```
  WRITE_REGISTER -4 1 8 # flash FCB
  ```

- JUMP\_ADDRESS \<address\>
  
  Description: The device jumps to the adress specified in the ADDRESS field.
  
- ERROR\_STATUS
  
  Description: When the device receives the ERROR\_STATUS command, it returns the global error status that is updated for each command.
