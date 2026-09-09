Traceback (most recent call last):
  File "D:\Modding\ShadowLimitFix\tools\disasm_uid.py", line 73, in <module>
    main()
    ~~~~^^
  File "D:\Modding\ShadowLimitFix\tools\disasm_uid.py", line 42, in main
    d, imgbase, sizeimg, sections = load_pe(path)
                                    ~~~~~~~^^^^^^
  File "D:\Modding\ShadowLimitFix\tools\disasm_uid.py", line 9, in load_pe
    d = open(path, 'rb').read()
        ~~~~^^^^^^^^^^^^
FileNotFoundError: [Errno 2] No such file or directory: 'E:/ENB-EJ/Skyrim Special Edition/SkyrimSE.exe'
