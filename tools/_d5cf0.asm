imagebase=0x140000000 size=0x3870000 sections=['.text', '.rdata', '.data', '.pdata', '.text', '_RDATA', '.rsrc', '.reloc', '.bind']
target VA=0x1414F5CF0 RVA=0x14F5CF0 fileoff=0x14F50F0 len=0x500
0x1414F5CF0: mov       qword ptr [rsp + 8], rbx
0x1414F5CF5: mov       qword ptr [rsp + 0x10], rbp
0x1414F5CFA: mov       qword ptr [rsp + 0x18], rsi
0x1414F5CFF: push      rdi
0x1414F5D00: sub       rsp, 0x20
0x1414F5D04: cmp       rdx, qword ptr [rip + 0x1e96de5]
0x1414F5D0B: mov       ebp, r9d
0x1414F5D0E: movzx     esi, r8b
0x1414F5D12: mov       rdi, rdx
0x1414F5D15: mov       rbx, rcx
0x1414F5D18: je        0x1414f5d70
0x1414F5D1A: test      bpl, 4
0x1414F5D1E: je        0x1414f5d47
0x1414F5D20: mov       ecx, dword ptr [rcx + 0x18]
0x1414F5D23: call      0x1414b4fc0   ; ->VA 0x1414B4FC0 (file 0x14B43C0)
0x1414F5D28: test      al, al
0x1414F5D2A: jne       0x1414f5d47
0x1414F5D2C: mov       rcx, rbx
0x1414F5D2F: call      0x1414e8e80   ; ->VA 0x1414E8E80 (file 0x14E8280)
0x1414F5D34: mov       r8, qword ptr [rbx + 8]
0x1414F5D38: mov       rdx, rax
0x1414F5D3B: movzx     r9d, sil
0x1414F5D3F: mov       rcx, rdi
0x1414F5D42: call      0x14150bc80   ; ->VA 0x14150BC80 (file 0x150B080)
0x1414F5D47: test      sil, sil
0x1414F5D4A: je        0x1414f5d70
0x1414F5D4C: mov       rcx, rbx
0x1414F5D4F: call      0x1414e8e80   ; ->VA 0x1414E8E80 (file 0x14E8280)
0x1414F5D54: test      rax, rax
0x1414F5D57: je        0x1414f5d70
0x1414F5D59: mov       rcx, rbx
0x1414F5D5C: call      0x1414e8e80   ; ->VA 0x1414E8E80 (file 0x14E8280)
0x1414F5D61: mov       r8, qword ptr [rbx + 8]
0x1414F5D65: mov       rdx, rax
0x1414F5D68: mov       rcx, rdi
0x1414F5D6B: call      0x14150bae0   ; ->VA 0x14150BAE0 (file 0x150AEE0)
0x1414F5D70: mov       rax, qword ptr [rdi]
0x1414F5D73: mov       r8d, ebp
0x1414F5D76: mov       rdx, rbx
0x1414F5D79: mov       rcx, rdi
0x1414F5D7C: mov       rbx, qword ptr [rsp + 0x30]
0x1414F5D81: mov       rbp, qword ptr [rsp + 0x38]
0x1414F5D86: mov       rsi, qword ptr [rsp + 0x40]
0x1414F5D8B: add       rsp, 0x20
0x1414F5D8F: pop       rdi
0x1414F5D90: jmp       qword ptr [rax + 0x30]
0x1414F5D94: int3      
0x1414F5D95: int3      
0x1414F5D96: int3      
0x1414F5D97: int3      
0x1414F5D98: int3      
0x1414F5D99: int3      
0x1414F5D9A: int3      
0x1414F5D9B: int3      
0x1414F5D9C: int3      
0x1414F5D9D: int3      
0x1414F5D9E: int3      
0x1414F5D9F: int3      
0x1414F5DA0: push      rdi
0x1414F5DA2: sub       rsp, 0x30
0x1414F5DA6: mov       qword ptr [rsp + 0x20], 0xfffffffffffffffe
0x1414F5DAF: mov       qword ptr [rsp + 0x48], rbx
0x1414F5DB4: mov       ecx, dword ptr [rip + 0x20faabe]
0x1414F5DBA: mov       rax, qword ptr gs:[0x58]
0x1414F5DC3: mov       edx, 0x768
0x1414F5DC8: mov       rdi, qword ptr [rax + rcx*8]
0x1414F5DCC: add       rdi, rdx
0x1414F5DCF: mov       ebx, dword ptr [rdi]
0x1414F5DD1: mov       dword ptr [rsp + 0x40], ebx
0x1414F5DD5: mov       dword ptr [rdi], 0x1a
0x1414F5DDB: cmp       byte ptr [rip + 0x20f921e], 0
0x1414F5DE2: jne       0x1414f5e3b
0x1414F5DE4: call      0x14147ac00   ; ->VA 0x14147AC00 (file 0x147A000)
0x1414F5DE9: call      0x1414a89c0   ; ->VA 0x1414A89C0 (file 0x14A7DC0)
0x1414F5DEE: call      0x14150df20   ; ->VA 0x14150DF20 (file 0x150D320)
0x1414F5DF3: call      0x14150e590   ; ->VA 0x14150E590 (file 0x150D990)
0x1414F5DF8: call      0x14150eb80   ; ->VA 0x14150EB80 (file 0x150DF80)
0x1414F5DFD: call      0x14150f1f0   ; ->VA 0x14150F1F0 (file 0x150E5F0)
0x1414F5E02: call      0x14150f850   ; ->VA 0x14150F850 (file 0x150EC50)
0x1414F5E07: cmp       qword ptr [rip + 0x1c91949], 0
0x1414F5E0F: jne       0x1414f5e24
0x1414F5E11: lea       rcx, [rip + 0x20f91f8]
0x1414F5E18: call      0x1414f7720   ; ->VA 0x1414F7720 (file 0x14F6B20)
0x1414F5E1D: mov       qword ptr [rip + 0x1c91934], rax
0x1414F5E24: mov       byte ptr [rip + 0x20f91d5], 1
0x1414F5E2B: call      0x1415089c0   ; ->VA 0x1415089C0 (file 0x1507DC0)
0x1414F5E30: call      0x1414e9f90   ; ->VA 0x1414E9F90 (file 0x14E9390)
0x1414F5E35: call      0x1414b8550   ; ->VA 0x1414B8550 (file 0x14B7950)
0x1414F5E3A: nop       
0x1414F5E3B: mov       dword ptr [rdi], ebx
0x1414F5E3D: mov       rbx, qword ptr [rsp + 0x48]
0x1414F5E42: add       rsp, 0x30
0x1414F5E46: pop       rdi
0x1414F5E47: ret       
0x1414F5E48: int3      
0x1414F5E49: int3      
0x1414F5E4A: int3      
0x1414F5E4B: int3      
0x1414F5E4C: int3      
0x1414F5E4D: int3      
0x1414F5E4E: int3      
0x1414F5E4F: int3      
0x1414F5E50: sub       rsp, 0x28
0x1414F5E54: cmp       byte ptr [rip + 0x20f91a5], 0
0x1414F5E5B: je        0x1414f5e81
0x1414F5E5D: call      0x14147ae10   ; ->VA 0x14147AE10 (file 0x147A210)
0x1414F5E62: call      0x1414a8ad0   ; ->VA 0x1414A8AD0 (file 0x14A7ED0)
0x1414F5E67: mov       byte ptr [rip + 0x20f9192], 0
0x1414F5E6E: call      0x141508a50   ; ->VA 0x141508A50 (file 0x1507E50)
0x1414F5E73: call      0x1414ea080   ; ->VA 0x1414EA080 (file 0x14E9480)
0x1414F5E78: add       rsp, 0x28
0x1414F5E7C: jmp       0x1414b8630   ; ->VA 0x1414B8630 (file 0x14B7A30)
0x1414F5E81: add       rsp, 0x28
0x1414F5E85: ret       
0x1414F5E86: int3      
0x1414F5E87: int3      
0x1414F5E88: int3      
0x1414F5E89: int3      
0x1414F5E8A: int3      
0x1414F5E8B: int3      
0x1414F5E8C: int3      
0x1414F5E8D: int3      
0x1414F5E8E: int3      
0x1414F5E8F: int3      
0x1414F5E90: mov       qword ptr [rip + 0x1c918c9], rcx
0x1414F5E97: lea       rax, [rcx + 0x10]
0x1414F5E9B: xor       edx, edx
0x1414F5E9D: mov       dword ptr [rcx + 4], edx
0x1414F5EA0: mov       qword ptr [rcx + 8], rax
0x1414F5EA4: mov       qword ptr [rax], rdx
0x1414F5EA7: lea       rax, [rcx + 0x20]
0x1414F5EAB: mov       qword ptr [rcx + 0x18], rax
0x1414F5EAF: mov       qword ptr [rax], rdx
0x1414F5EB2: mov       rax, rcx
0x1414F5EB5: mov       qword ptr [rcx + 0x28], rdx
0x1414F5EB9: ret       
0x1414F5EBA: int3      
0x1414F5EBB: int3      
0x1414F5EBC: int3      
0x1414F5EBD: int3      
0x1414F5EBE: int3      
0x1414F5EBF: int3      
0x1414F5EC0: mov       qword ptr [rcx + 0x28], rdx
0x1414F5EC4: ret       
0x1414F5EC5: int3      
0x1414F5EC6: int3      
0x1414F5EC7: int3      
0x1414F5EC8: int3      
0x1414F5EC9: int3      
0x1414F5ECA: int3      
0x1414F5ECB: int3      
0x1414F5ECC: int3      
0x1414F5ECD: int3      
0x1414F5ECE: int3      
0x1414F5ECF: int3      
0x1414F5ED0: mov       rax, rsp
0x1414F5ED3: push      r12
0x1414F5ED5: push      r14
0x1414F5ED7: push      r15
0x1414F5ED9: sub       rsp, 0x270
0x1414F5EE0: mov       qword ptr [rsp + 0x48], 0xfffffffffffffffe
0x1414F5EE9: mov       qword ptr [rax + 8], rbx
0x1414F5EED: mov       qword ptr [rax + 0x10], rbp
0x1414F5EF1: mov       qword ptr [rax + 0x18], rsi
0x1414F5EF5: mov       qword ptr [rax + 0x20], rdi
0x1414F5EF9: mov       r12, r9
0x1414F5EFC: mov       r15, r8
0x1414F5EFF: mov       ecx, dword ptr [rip + 0x20fa973]
0x1414F5F05: mov       rax, qword ptr gs:[0x58]
0x1414F5F0E: mov       rdi, qword ptr [rax + rcx*8]
0x1414F5F12: mov       r14d, 0x768
0x1414F5F18: add       r14, rdi
0x1414F5F1B: mov       ebx, dword ptr [r14]
0x1414F5F1E: mov       dword ptr [rsp + 0x20], ebx
0x1414F5F22: mov       dword ptr [r14], 0x1c
0x1414F5F29: mov       bpl, 1
0x1414F5F2C: mov       r9, rdx
0x1414F5F2F: lea       r8, [rip + 0x5cb272]
0x1414F5F36: mov       edx, 0x104
0x1414F5F3B: lea       rcx, [rsp + 0x50]
0x1414F5F40: call      0x14018a900   ; ->VA 0x14018A900 (file 0x189D00)
0x1414F5F45: mov       eax, 0x2a08
0x1414F5F4A: mov       eax, dword ptr [rax + rdi]
0x1414F5F4D: cmp       dword ptr [rip + 0x20f90fd], eax
0x1414F5F53: jg        0x1414f603c
0x1414F5F59: lea       r8, [rip + 0xb3f8f8]
0x1414F5F60: lea       rdx, [rsp + 0x50]
0x1414F5F65: lea       rcx, [rsp + 0x28]
0x1414F5F6A: call      0x140e1f1f0   ; ->VA 0x140E1F1F0 (file 0xE1E5F0)
0x1414F5F6F: nop       
0x1414F5F70: cmp       byte ptr [rsp + 0x38], 0
0x1414F5F75: je        0x1414f5fe7
0x1414F5F77: mov       rax, qword ptr [rsp + 0x30]
0x1414F5F7C: test      rax, rax
0x1414F5F7F: je        0x1414f5f86
0x1414F5F81: mov       esi, dword ptr [rax + 8]
0x1414F5F84: jmp       0x1414f5f88   ; ->VA 0x1414F5F88 (file 0x14F5388)
0x1414F5F86: xor       esi, esi
0x1414F5F88: cmp       dword ptr [rip + 0x20fb241], 2
0x1414F5F8F: je        0x1414f5fa4
0x1414F5F91: lea       rdx, [rip + 0x20fb238]
0x1414F5F98: lea       rcx, [rip + 0x20fb241]
0x1414F5F9F: call      0x140cc4680   ; ->VA 0x140CC4680 (file 0xCC3A80)
0x1414F5FA4: lea       edi, [rsi + 1]
0x1414F5FA7: xor       edx, edx
0x1414F5FA9: lea       rcx, [rip + 0x20fb230]
0x1414F5FB0: call      0x140cc3660   ; ->VA 0x140CC3660 (file 0xCC2A60)
0x1414F5FB5: mov       rcx, rax
0x1414F5FB8: mov       r8d, 8
0x1414F5FBE: mov       edx, edi
0x1414F5FC0: call      0x140cc54d0   ; ->VA 0x140CC54D0 (file 0xCC48D0)
0x1414F5FC5: mov       qword ptr [r15], rax
0x1414F5FC8: mov       r8d, esi
0x1414F5FCB: mov       rdx, rax
0x1414F5FCE: lea       rcx, [rsp + 0x30]
0x1414F5FD3: call      0x140e1f0e0   ; ->VA 0x140E1F0E0 (file 0xE1E4E0)
0x1414F5FD8: mov       ecx, esi
0x1414F5FDA: mov       rax, qword ptr [r15]
0x1414F5FDD: mov       byte ptr [rcx + rax], 0
0x1414F5FE1: mov       dword ptr [r12], esi
0x1414F5FE5: jmp       0x1414f6008   ; ->VA 0x1414F6008 (file 0x14F5408)
0x1414F5FE7: lea       r9, [rsp + 0x50]
0x1414F5FEC: lea       r8, [rip + 0x5cb1c5]
0x1414F5FF3: mov       edx, 0x104
0x1414F5FF8: lea       rcx, [rsp + 0x160]
0x1414F6000: call      0x14018a900   ; ->VA 0x14018A900 (file 0x189D00)
0x1414F6005: xor       bpl, bpl
0x1414F6008: lea       rcx, [rsp + 0x28]
0x1414F600D: call      0x140e1f3e0   ; ->VA 0x140E1F3E0 (file 0xE1E7E0)
0x1414F6012: nop       
0x1414F6013: mov       dword ptr [r14], ebx
0x1414F6016: movzx     eax, bpl
0x1414F601A: lea       r11, [rsp + 0x270]
0x1414F6022: mov       rbx, qword ptr [r11 + 0x20]
0x1414F6026: mov       rbp, qword ptr [r11 + 0x28]
0x1414F602A: mov       rsi, qword ptr [r11 + 0x30]
0x1414F602E: mov       rdi, qword ptr [r11 + 0x38]
0x1414F6032: mov       rsp, r11
0x1414F6035: pop       r15
0x1414F6037: pop       r14
0x1414F6039: pop       r12
0x1414F603B: ret       
0x1414F603C: lea       rcx, [rip + 0x20f900d]
0x1414F6043: call      0x14153b45c   ; ->VA 0x14153B45C (file 0x153A85C)
0x1414F6048: cmp       dword ptr [rip + 0x20f9001], -1
0x1414F604F: jne       0x1414f5f59
0x1414F6055: mov       byte ptr [rip + 0xb3f804], 0
0x1414F605C: lea       rax, [rip + 0x2901fd]
0x1414F6063: mov       qword ptr [rip + 0xb3f7ee], rax
0x1414F606A: lea       rdx, [rip + 0x290267]
0x1414F6071: lea       rcx, [rip + 0xb3f7f0]
0x1414F6078: call      0x140cec5d0   ; ->VA 0x140CEC5D0 (file 0xCEB9D0)
0x1414F607D: mov       dword ptr [rip + 0xb3f7e9], 0x10000
0x1414F6087: mov       byte ptr [rip + 0xb3f7e6], 0
0x1414F608E: lea       rcx, [rip + 0x257adb]
0x1414F6095: call      0x14153b080   ; ->VA 0x14153B080 (file 0x153A480)
0x1414F609A: nop       
0x1414F609B: lea       rcx, [rip + 0x20f8fae]
0x1414F60A2: call      0x14153b3fc   ; ->VA 0x14153B3FC (file 0x153A7FC)
0x1414F60A7: jmp       0x1414f5f59   ; ->VA 0x1414F5F59 (file 0x14F5359)
0x1414F60AC: int3      
0x1414F60AD: int3      
0x1414F60AE: int3      
0x1414F60AF: int3      
0x1414F60B0: int3      
0x1414F60B1: int3      
0x1414F60B2: int3      
0x1414F60B3: int3      
0x1414F60B4: int3      
0x1414F60B5: int3      
0x1414F60B6: int3      
0x1414F60B7: int3      
0x1414F60B8: int3      
0x1414F60B9: int3      
0x1414F60BA: int3      
0x1414F60BB: int3      
0x1414F60BC: int3      
0x1414F60BD: int3      
0x1414F60BE: int3      
0x1414F60BF: int3      
0x1414F60C0: push      rbx
0x1414F60C2: sub       rsp, 0x20
0x1414F60C6: cmp       dword ptr [rip + 0x20fb103], 2
0x1414F60CD: mov       rbx, rdx
0x1414F60D0: je        0x1414f60e5
0x1414F60D2: lea       rdx, [rip + 0x20fb0f7]
0x1414F60D9: lea       rcx, [rip + 0x20fb100]
0x1414F60E0: call      0x140cc4680   ; ->VA 0x140CC4680 (file 0xCC3A80)
0x1414F60E5: xor       edx, edx
0x1414F60E7: lea       rcx, [rip + 0x20fb0f2]
0x1414F60EE: call      0x140cc3660   ; ->VA 0x140CC3660 (file 0xCC2A60)
0x1414F60F3: mov       rcx, rax
0x1414F60F6: mov       rdx, rbx
0x1414F60F9: add       rsp, 0x20
0x1414F60FD: pop       rbx
0x1414F60FE: jmp       0x140cc5ad0   ; ->VA 0x140CC5AD0 (file 0xCC4ED0)
0x1414F6103: int3      
0x1414F6104: int3      
0x1414F6105: int3      
0x1414F6106: int3      
0x1414F6107: int3      
0x1414F6108: int3      
0x1414F6109: int3      
0x1414F610A: int3      
0x1414F610B: int3      
0x1414F610C: int3      
0x1414F610D: int3      
0x1414F610E: int3      
0x1414F610F: int3      
0x1414F6110: mov       qword ptr [rsp + 8], rbp
0x1414F6115: mov       qword ptr [rsp + 0x10], rsi
0x1414F611A: mov       qword ptr [rsp + 0x18], rdi
0x1414F611F: mov       qword ptr [rsp + 0x20], r14
0x1414F6124: push      r15
0x1414F6126: sub       rsp, 0x140
0x1414F612D: mov       rax, qword ptr [rsp + 0x178]
0x1414F6135: lea       rdi, [rip + 0x5d5b84]
0x1414F613C: test      rax, rax
0x1414F613F: mov       r15, r9
0x1414F6142: mov       esi, r8d
0x1414F6145: mov       rbp, rdx
0x1414F6148: cmovne    rdi, rax
0x1414F614C: mov       r14, rcx
0x1414F614F: mov       r9, rdx
0x1414F6152: mov       qword ptr [rsp + 0x28], rdi
0x1414F6157: mov       dword ptr [rsp + 0x20], r8d
0x1414F615C: lea       rcx, [rsp + 0x30]
0x1414F6161: lea       r8, [rip + 0x5cb078]
0x1414F6168: mov       edx, 0x104
0x1414F616D: call      0x14018a900   ; ->VA 0x14018A900 (file 0x189D00)
0x1414F6172: mov       r9, qword ptr [rsp + 0x170]
0x1414F617A: lea       rdx, [rsp + 0x30]
0x1414F617F: mov       r8, r15
0x1414F6182: mov       rcx, r14
0x1414F6185: call      0x1414f63a0   ; ->VA 0x1414F63A0 (file 0x14F57A0)
0x1414F618A: mov       r9, rbp
0x1414F618D: mov       qword ptr [rsp + 0x28], rdi
0x1414F6192: lea       r8, [rip + 0x5cb067]
0x1414F6199: mov       dword ptr [rsp + 0x20], esi
0x1414F619D: mov       edx, 0x104
0x1414F61A2: lea       rcx, [rsp + 0x30]
0x1414F61A7: call      0x14018a900   ; ->VA 0x14018A900 (file 0x189D00)
0x1414F61AC: mov       r9, qword ptr [rsp + 0x170]
0x1414F61B4: lea       rdx, [rsp + 0x30]
0x1414F61B9: mov       r8, r15
0x1414F61BC: mov       rcx, r14
0x1414F61BF: call      0x1414f63a0   ; ->VA 0x1414F63A0 (file 0x14F57A0)
0x1414F61C4: lea       r11, [rsp + 0x140]
0x1414F61CC: mov       rbp, qword ptr [r11 + 0x10]
0x1414F61D0: mov       rsi, qword ptr [r11 + 0x18]
0x1414F61D4: mov       rdi, qword ptr [r11 + 0x20]
0x1414F61D8: mov       r14, qword ptr [r11 + 0x28]
0x1414F61DC: mov       rsp, r11
0x1414F61DF: pop       r15
0x1414F61E1: ret       
0x1414F61E2: int3      
0x1414F61E3: int3      
0x1414F61E4: int3      
0x1414F61E5: int3      
0x1414F61E6: int3      
0x1414F61E7: int3      
0x1414F61E8: int3      
0x1414F61E9: int3      
0x1414F61EA: int3      
0x1414F61EB: int3      
0x1414F61EC: int3      
0x1414F61ED: int3      
0x1414F61EE: int3      
0x1414F61EF: int3      
