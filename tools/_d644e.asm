imagebase=0x140000000 size=0x3870000 sections=['.text', '.rdata', '.data', '.pdata', '.text', '_RDATA', '.rsrc', '.reloc', '.bind']
target VA=0x1414F3DC0 RVA=0x14F3DC0 fileoff=0x14F31C0 len=0x3A0
0x1414F3DC0: mov       rax, rsp
0x1414F3DC3: push      rdi
0x1414F3DC4: push      r12
0x1414F3DC6: push      r13
0x1414F3DC8: push      r14
0x1414F3DCA: push      r15
0x1414F3DCC: sub       rsp, 0x80
0x1414F3DD3: mov       qword ptr [rsp + 0x20], 0xfffffffffffffffe
0x1414F3DDC: mov       qword ptr [rax + 8], rbx
0x1414F3DE0: mov       qword ptr [rax + 0x18], rbp
0x1414F3DE4: mov       qword ptr [rax + 0x20], rsi
0x1414F3DE8: mov       ebp, r9d
0x1414F3DEB: movzx     r12d, r8b
0x1414F3DEF: mov       ebx, edx
0x1414F3DF1: mov       rsi, rcx
0x1414F3DF4: mov       rdi, qword ptr [rcx]
0x1414F3DF7: mov       r14, qword ptr [rcx + 0x10]
0x1414F3DFB: xor       r15d, r15d
0x1414F3DFE: mov       rcx, qword ptr [rip + 0x20f61d3]
0x1414F3E05: mov       edx, dword ptr [rip + 0x20f61c9]
0x1414F3E0B: cmp       edx, ebx
0x1414F3E0D: jne       0x1414f3e1c
0x1414F3E0F: cmp       ebx, 0x5c006076
0x1414F3E15: je        0x1414f3e1c
0x1414F3E17: cmp       rdi, rcx
0x1414F3E1A: je        0x1414f3e62
0x1414F3E1C: mov       dword ptr [rip + 0xb3f2fa], ebx
0x1414F3E22: test      rcx, rcx
0x1414F3E25: je        0x1414f3e2d
0x1414F3E27: mov       rax, qword ptr [rcx]
0x1414F3E2A: call      qword ptr [rax + 0x18]
0x1414F3E2D: mov       qword ptr [rip + 0x20f61a4], r15
0x1414F3E34: mov       dword ptr [rip + 0x20f6199], r15d
0x1414F3E3B: mov       qword ptr [rip + 0x20f619e], r15
0x1414F3E42: mov       rax, qword ptr [rdi]
0x1414F3E45: mov       edx, ebx
0x1414F3E47: mov       rcx, rdi
0x1414F3E4A: call      qword ptr [rax + 0x10]
0x1414F3E4D: test      al, al
0x1414F3E4F: je        0x1414f40f2
0x1414F3E55: mov       qword ptr [rip + 0x20f617c], rdi
0x1414F3E5C: mov       dword ptr [rip + 0x20f6172], ebx
0x1414F3E62: mov       rbx, qword ptr [rsi + 8]
0x1414F3E66: test      rbx, rbx
0x1414F3E69: je        0x1414f3e71
0x1414F3E6B: mov       rbx, qword ptr [rbx + 0x78]
0x1414F3E6F: jmp       0x1414f3e74   ; ->VA 0x1414F3E74 (file 0x14F3274)
0x1414F3E71: mov       rbx, r15
0x1414F3E74: cmp       rbx, qword ptr [rip + 0x20f6165]
0x1414F3E7B: je        0x1414f3e95
0x1414F3E7D: test      rbx, rbx
0x1414F3E80: je        0x1414f3e8e
0x1414F3E82: mov       rax, qword ptr [rdi]
0x1414F3E85: mov       rdx, rbx
0x1414F3E88: mov       rcx, rdi
0x1414F3E8B: call      qword ptr [rax + 0x20]
0x1414F3E8E: mov       qword ptr [rip + 0x20f614b], rbx
0x1414F3E95: movzx     eax, byte ptr [rsi + 0x1e]
0x1414F3E99: mov       byte ptr [r14 + 0x108], al
0x1414F3EA0: cmp       qword ptr [r14 + 0x130], r15
0x1414F3EA7: je        0x1414f400e
0x1414F3EAD: mov       rdi, qword ptr [rsi + 0x10]
0x1414F3EB1: mov       r14, qword ptr [rsi]
0x1414F3EB4: mov       r15, qword ptr [rdi + 0x130]
0x1414F3EBB: call      0x14150bfc0   ; ->VA 0x14150BFC0 (file 0x150B3C0)
0x1414F3EC0: mov       rax, qword ptr [rdi]
0x1414F3EC3: mov       rcx, rdi
0x1414F3EC6: call      qword ptr [rax + 0x1b0]
0x1414F3ECC: mov       r9d, ebp
0x1414F3ECF: movzx     r8d, r12b
0x1414F3ED3: mov       rdx, r14
0x1414F3ED6: mov       rcx, rsi
0x1414F3ED9: test      rax, rax
0x1414F3EDC: jne       0x1414f3fb3
0x1414F3EE2: call      0x1414f5cf0   ; ->VA 0x1414F5CF0 (file 0x14F50F0)
0x1414F3EE7: movzx     ebx, byte ptr [rsi + 0x1e]
0x1414F3EEB: mov       rax, qword ptr [rdi]
0x1414F3EEE: mov       rcx, rdi
0x1414F3EF1: call      qword ptr [rax + 0x60]
0x1414F3EF4: mov       r13, rax
0x1414F3EF7: lea       rcx, [r14 + 0x10]
0x1414F3EFB: test      r14, r14
0x1414F3EFE: mov       eax, 0
0x1414F3F03: cmove     rcx, rax
0x1414F3F07: mov       qword ptr [rsp + 0x30], rcx
0x1414F3F0C: mov       qword ptr [rsp + 0x38], rdi
0x1414F3F11: mov       qword ptr [rsp + 0x40], rax
0x1414F3F16: mov       ecx, ebx
0x1414F3F18: shr       ecx, 7
0x1414F3F1B: mov       dword ptr [rsp + 0x48], ecx
0x1414F3F1F: btr       ebx, 7
0x1414F3F23: mov       dword ptr [rsp + 0x4c], ebx
0x1414F3F27: xorps     xmm0, xmm0
0x1414F3F2A: movss     dword ptr [rsp + 0x50], xmm0
0x1414F3F30: mov       dword ptr [rsp + 0x54], 0xffffffff
0x1414F3F38: test      r13, r13
0x1414F3F3B: je        0x1414f3f8e
0x1414F3F3D: lea       r8, [rsp + 0x54]
0x1414F3F42: mov       edx, dword ptr [r13 + 0x170]
0x1414F3F49: lea       rcx, [rip + 0x1d94870]
0x1414F3F50: call      0x140e46d60   ; ->VA 0x140E46D60 (file 0xE46160)
0x1414F3F55: mov       rdi, rax
0x1414F3F58: mov       ebx, dword ptr [r13 + 0x170]
0x1414F3F5F: mov       rcx, r13
0x1414F3F62: call      0x140d38c60   ; ->VA 0x140D38C60 (file 0xD38060)
0x1414F3F67: mov       r8, rax
0x1414F3F6A: mov       r9d, ebx
0x1414F3F6D: mov       edx, ebx
0x1414F3F6F: mov       rcx, rdi
0x1414F3F72: call      0x1401099d0   ; ->VA 0x1401099D0 (file 0x108DD0)
0x1414F3F77: mov       rdx, rdi
0x1414F3F7A: lea       rcx, [rip + 0x1d9483f]
0x1414F3F81: call      0x140e46ea0   ; ->VA 0x140E46EA0 (file 0xE462A0)
0x1414F3F86: mov       rcx, r13
0x1414F3F89: call      0x140d38cc0   ; ->VA 0x140D38CC0 (file 0xD380C0)
0x1414F3F8E: mov       rax, qword ptr [r15]
0x1414F3F91: lea       rdx, [rsp + 0x30]
0x1414F3F96: mov       rcx, r15
0x1414F3F99: call      qword ptr [rax + 0x128]
0x1414F3F9F: mov       rax, qword ptr [r14]
0x1414F3FA2: mov       r8d, ebp
0x1414F3FA5: mov       rdx, rsi
0x1414F3FA8: mov       rcx, r14
0x1414F3FAB: call      qword ptr [rax + 0x38]
0x1414F3FAE: jmp       0x1414f4100   ; ->VA 0x1414F4100 (file 0x14F3500)
0x1414F3FB3: call      0x1414f5cf0   ; ->VA 0x1414F5CF0 (file 0x14F50F0)
0x1414F3FB8: lea       rcx, [rsp + 0x30]
0x1414F3FBD: call      0x140d429e0   ; ->VA 0x140D429E0 (file 0xD41DE0)
0x1414F3FC2: nop       
0x1414F3FC3: mov       eax, 1
0x1414F3FC8: mov       word ptr [rsp + 0x6c], ax
0x1414F3FCD: lea       rcx, [r14 + 0x10]
0x1414F3FD1: mov       rax, qword ptr [rcx]
0x1414F3FD4: lea       r9, [rdi + 0x7c]
0x1414F3FD8: lea       r8, [rsp + 0x30]
0x1414F3FDD: mov       rdx, qword ptr [rdi + 0x130]
0x1414F3FE4: call      qword ptr [rax + 8]
0x1414F3FE7: mov       rcx, rsi
0x1414F3FEA: call      0x1414f2ad0   ; ->VA 0x1414F2AD0 (file 0x14F1ED0)
0x1414F3FEF: nop       
0x1414F3FF0: lea       rcx, [rsp + 0x30]
0x1414F3FF5: call      0x140d42a20   ; ->VA 0x140D42A20 (file 0xD41E20)
0x1414F3FFA: mov       rax, qword ptr [r14]
0x1414F3FFD: mov       r8d, ebp
0x1414F4000: mov       rdx, rsi
0x1414F4003: mov       rcx, r14
0x1414F4006: call      qword ptr [rax + 0x38]
0x1414F4009: jmp       0x1414f4100   ; ->VA 0x1414F4100 (file 0x14F3500)
0x1414F400E: test      byte ptr [r14 + 0x109], 8
0x1414F4016: je        0x1414f4081
0x1414F4018: mov       rbx, qword ptr [rsi]
0x1414F401B: mov       rax, qword ptr [rbx]
0x1414F401E: mov       r8d, ebp
0x1414F4021: mov       rdx, rsi
0x1414F4024: mov       rcx, rbx
0x1414F4027: call      qword ptr [rax + 0x30]
0x1414F402A: mov       rcx, rsi
0x1414F402D: call      0x1414e8e80   ; ->VA 0x1414E8E80 (file 0x14E8280)
0x1414F4032: mov       rdx, rax
0x1414F4035: mov       r9b, 1
0x1414F4038: mov       r8, qword ptr [rsi + 8]
0x1414F403C: mov       rcx, rbx
0x1414F403F: call      0x14150bc80   ; ->VA 0x14150BC80 (file 0x150B080)
0x1414F4044: mov       rcx, rsi
0x1414F4047: call      0x1414e8e80   ; ->VA 0x1414E8E80 (file 0x14E8280)
0x1414F404C: test      rax, rax
0x1414F404F: je        0x1414f4068
0x1414F4051: mov       rcx, rsi
0x1414F4054: call      0x1414e8e80   ; ->VA 0x1414E8E80 (file 0x14E8280)
0x1414F4059: mov       rdx, rax
0x1414F405C: mov       r8, qword ptr [rsi + 8]
0x1414F4060: mov       rcx, rbx
0x1414F4063: call      0x14150bae0   ; ->VA 0x14150BAE0 (file 0x150AEE0)
0x1414F4068: mov       rcx, rsi
0x1414F406B: call      0x1414f2ad0   ; ->VA 0x1414F2AD0 (file 0x14F1ED0)
0x1414F4070: mov       rax, qword ptr [rbx]
0x1414F4073: mov       r8d, ebp
0x1414F4076: mov       rdx, rsi
0x1414F4079: mov       rcx, rbx
0x1414F407C: call      qword ptr [rax + 0x38]
0x1414F407F: jmp       0x1414f4100   ; ->VA 0x1414F4100 (file 0x14F3500)
0x1414F4081: mov       ecx, dword ptr [rip + 0x20fc7f1]
0x1414F4087: mov       rax, qword ptr gs:[0x58]
0x1414F4090: mov       r15d, 0x768
0x1414F4096: mov       rdi, qword ptr [rax + rcx*8]
0x1414F409A: mov       ebx, dword ptr [rdi + r15]
0x1414F409E: mov       dword ptr [rsp + 0xb8], ebx
0x1414F40A5: mov       dword ptr [rdi + r15], 0x1a
0x1414F40AD: mov       r14, qword ptr [rsi]
0x1414F40B0: test      r12b, r12b
0x1414F40B3: jne       0x1414f40c3
0x1414F40B5: cmp       byte ptr [rip + 0x1d98bbd], r12b
0x1414F40BC: jne       0x1414f40c3
0x1414F40BE: xor       r8d, r8d
0x1414F40C1: jmp       0x1414f40c6   ; ->VA 0x1414F40C6 (file 0x14F34C6)
0x1414F40C3: mov       r8b, 1
0x1414F40C6: mov       r9d, ebp
0x1414F40C9: mov       rdx, r14
0x1414F40CC: mov       rcx, rsi
0x1414F40CF: call      0x1414f5cf0   ; ->VA 0x1414F5CF0 (file 0x14F50F0)
0x1414F40D4: mov       rcx, rsi
0x1414F40D7: call      0x1414f2ad0   ; ->VA 0x1414F2AD0 (file 0x14F1ED0)
0x1414F40DC: mov       rax, qword ptr [r14]
0x1414F40DF: mov       r8d, ebp
0x1414F40E2: mov       rdx, rsi
0x1414F40E5: mov       rcx, r14
0x1414F40E8: call      qword ptr [rax + 0x38]
0x1414F40EB: nop       
0x1414F40EC: mov       dword ptr [rdi + r15], ebx
0x1414F40F0: jmp       0x1414f4100   ; ->VA 0x1414F4100 (file 0x14F3500)
0x1414F40F2: mov       qword ptr [rip + 0x20f5edf], r15
0x1414F40F9: mov       dword ptr [rip + 0x20f5ed4], r15d
0x1414F4100: lea       r11, [rsp + 0x80]
0x1414F4108: mov       rbx, qword ptr [r11 + 0x30]
0x1414F410C: mov       rbp, qword ptr [r11 + 0x40]
0x1414F4110: mov       rsi, qword ptr [r11 + 0x48]
0x1414F4114: mov       rsp, r11
0x1414F4117: pop       r15
0x1414F4119: pop       r14
0x1414F411B: pop       r13
0x1414F411D: pop       r12
0x1414F411F: pop       rdi
0x1414F4120: ret       
0x1414F4121: int3      
0x1414F4122: int3      
0x1414F4123: int3      
0x1414F4124: int3      
0x1414F4125: int3      
0x1414F4126: int3      
0x1414F4127: int3      
0x1414F4128: int3      
0x1414F4129: int3      
0x1414F412A: int3      
0x1414F412B: int3      
0x1414F412C: int3      
0x1414F412D: int3      
0x1414F412E: int3      
0x1414F412F: int3      
0x1414F4130: xor       eax, eax
0x1414F4132: mov       qword ptr [rip + 0x20f5e9f], rax
0x1414F4139: mov       dword ptr [rip + 0x20f5e95], eax
0x1414F413F: mov       qword ptr [rip + 0x20f5e9a], rax
0x1414F4146: ret       
0x1414F4147: int3      
0x1414F4148: int3      
0x1414F4149: int3      
0x1414F414A: int3      
0x1414F414B: int3      
0x1414F414C: int3      
0x1414F414D: int3      
0x1414F414E: int3      
0x1414F414F: int3      
0x1414F4150: mov       dword ptr [rip + 0x20f5e76], 0
0x1414F415A: ret       
0x1414F415B: int3      
0x1414F415C: int3      
0x1414F415D: int3      
0x1414F415E: int3      
0x1414F415F: int3      
