imagebase=0x140000000 size=0x3870000 sections=['.text', '.rdata', '.data', '.pdata', '.text', '_RDATA', '.rsrc', '.reloc', '.bind']
target VA=0x1406DC570 RVA=0x6DC570 fileoff=0x6DB970 len=0x898
0x1406DC570: add       dword ptr [rax], eax
0x1406DC572: add       byte ptr [rax], al
0x1406DC574: call      qword ptr [rax]
0x1406DC576: movzx     eax, byte ptr [rdi + 0x83]
0x1406DC57D: and       al, 0xfe
0x1406DC57F: or        al, 2
0x1406DC581: mov       byte ptr [rdi + 0x83], al
0x1406DC587: mov       qword ptr [rbp - 0x31], rdi
0x1406DC58B: lock inc  dword ptr [rdi + 8]
0x1406DC58F: lea       rdx, [rbp - 0x31]
0x1406DC593: mov       rcx, r12
0x1406DC596: call      0x140692b40   ; ->VA 0x140692B40 (file 0x691F40)
0x1406DC59B: nop       
0x1406DC59C: mov       rcx, qword ptr [rbp - 0x31]
0x1406DC5A0: test      rcx, rcx
0x1406DC5A3: je        0x1406dc5bb
0x1406DC5A5: mov       eax, esi
0x1406DC5A7: lock xadd dword ptr [rcx + 8], eax
0x1406DC5AC: cmp       eax, 1
0x1406DC5AF: jne       0x1406dc5bb
0x1406DC5B1: mov       rax, qword ptr [rcx]
0x1406DC5B4: mov       edx, 1
0x1406DC5B9: call      qword ptr [rax]
0x1406DC5BB: mov       rax, qword ptr [rbp + 0x77]
0x1406DC5BF: mov       dword ptr [rax + 8], 1
0x1406DC5C6: lock xadd dword ptr [rdi + 8], esi
0x1406DC5CB: cmp       esi, 1
0x1406DC5CE: jne       0x1406dc5db
0x1406DC5D0: mov       rax, qword ptr [rdi]
0x1406DC5D3: mov       edx, esi
0x1406DC5D5: mov       rcx, rdi
0x1406DC5D8: call      qword ptr [rax]
0x1406DC5DA: nop       
0x1406DC5DB: mov       eax, dword ptr [rbp + 0x7f]
0x1406DC5DE: mov       dword ptr [r15 + r13], eax
0x1406DC5E2: jmp       0x1406dc73a   ; ->VA 0x1406DC73A (file 0x6DBB3A)
0x1406DC5E7: mov       r15, qword ptr [rbp - 0x61]
0x1406DC5EB: test      r13, r13
0x1406DC5EE: jne       0x1406dc5f9
0x1406DC5F0: test      r15, r15
0x1406DC5F3: je        0x1406dc73a
0x1406DC5F9: mov       ecx, dword ptr [rip + 0x2f14279]
0x1406DC5FF: mov       rax, qword ptr gs:[0x58]
0x1406DC608: mov       edx, 0x768
0x1406DC60D: mov       edi, edx
0x1406DC60F: mov       r14, qword ptr [rax + rcx*8]
0x1406DC613: mov       eax, dword ptr [r14 + rdx]
0x1406DC617: mov       dword ptr [rbp - 0x69], eax
0x1406DC61A: mov       dword ptr [r14 + rdx], 0x4e
0x1406DC622: cmp       dword ptr [rip + 0x2f14ba7], 2
0x1406DC629: je        0x1406dc63e
0x1406DC62B: lea       rdx, [rip + 0x2f14b9e]
0x1406DC632: lea       rcx, [rip + 0x2f14ba7]
0x1406DC639: call      0x140cc4680   ; ->VA 0x140CC4680 (file 0xCC3A80)
0x1406DC63E: xor       r9d, r9d
0x1406DC641: xor       r8d, r8d
0x1406DC644: mov       edx, 0x100
0x1406DC649: lea       rcx, [rip + 0x2f14b90]
0x1406DC650: call      0x140cc40c0   ; ->VA 0x140CC40C0 (file 0xCC34C0)
0x1406DC655: mov       qword ptr [rbp - 0x59], rax
0x1406DC659: test      rax, rax
0x1406DC65C: je        0x1406dc66b
0x1406DC65E: mov       rcx, rax
0x1406DC661: call      0x1404dba50   ; ->VA 0x1404DBA50 (file 0x4DAE50)
0x1406DC666: mov       rbx, rax
0x1406DC669: jmp       0x1406dc66d   ; ->VA 0x1406DC66D (file 0x6DBA6D)
0x1406DC66B: xor       ebx, ebx
0x1406DC66D: mov       qword ptr [rbp + 7], rbx
0x1406DC671: test      rbx, rbx
0x1406DC674: je        0x1406dc67a
0x1406DC676: lock inc  dword ptr [rbx + 8]
0x1406DC67A: mov       qword ptr [rbp - 0x29], rbx
0x1406DC67E: test      rbx, rbx
0x1406DC681: je        0x1406dc687
0x1406DC683: lock inc  dword ptr [rbx + 8]
0x1406DC687: mov       qword ptr [rsp + 0x30], 0
0x1406DC690: movss     dword ptr [rsp + 0x28], xmm6
0x1406DC696: mov       qword ptr [rsp + 0x20], r15
0x1406DC69B: mov       r9, r13
0x1406DC69E: lea       r8, [rbp - 0x19]
0x1406DC6A2: lea       rdx, [rbp - 0x29]
0x1406DC6A6: mov       rcx, r12
0x1406DC6A9: call      0x1406937c0   ; ->VA 0x1406937C0 (file 0x692BC0)
0x1406DC6AE: nop       
0x1406DC6AF: mov       rcx, qword ptr [rbp - 0x29]
0x1406DC6B3: test      rcx, rcx
0x1406DC6B6: je        0x1406dc6ce
0x1406DC6B8: mov       eax, esi
0x1406DC6BA: lock xadd dword ptr [rcx + 8], eax
0x1406DC6BF: cmp       eax, 1
0x1406DC6C2: jne       0x1406dc6ce
0x1406DC6C4: mov       rax, qword ptr [rcx]
0x1406DC6C7: mov       edx, 1
0x1406DC6CC: call      qword ptr [rax]
0x1406DC6CE: movzx     eax, byte ptr [rbx + 0x83]
0x1406DC6D5: and       al, 0xfe
0x1406DC6D7: or        al, 2
0x1406DC6D9: mov       byte ptr [rbx + 0x83], al
0x1406DC6DF: mov       qword ptr [rbp - 0x21], rbx
0x1406DC6E3: lock inc  dword ptr [rbx + 8]
0x1406DC6E7: lea       rdx, [rbp - 0x21]
0x1406DC6EB: mov       rcx, r12
0x1406DC6EE: call      0x140692b40   ; ->VA 0x140692B40 (file 0x691F40)
0x1406DC6F3: nop       
0x1406DC6F4: mov       rcx, qword ptr [rbp - 0x21]
0x1406DC6F8: test      rcx, rcx
0x1406DC6FB: je        0x1406dc713
0x1406DC6FD: mov       eax, esi
0x1406DC6FF: lock xadd dword ptr [rcx + 8], eax
0x1406DC704: cmp       eax, 1
0x1406DC707: jne       0x1406dc713
0x1406DC709: mov       rax, qword ptr [rcx]
0x1406DC70C: mov       edx, 1
0x1406DC711: call      qword ptr [rax]
0x1406DC713: mov       rax, qword ptr [rbp + 0x77]
0x1406DC717: mov       dword ptr [rax + 8], 1
0x1406DC71E: lock xadd dword ptr [rbx + 8], esi
0x1406DC723: cmp       esi, 1
0x1406DC726: jne       0x1406dc733
0x1406DC728: mov       rax, qword ptr [rbx]
0x1406DC72B: mov       edx, esi
0x1406DC72D: mov       rcx, rbx
0x1406DC730: call      qword ptr [rax]
0x1406DC732: nop       
0x1406DC733: mov       eax, dword ptr [rbp - 0x69]
0x1406DC736: mov       dword ptr [r14 + rdi], eax
0x1406DC73A: mov       rbx, qword ptr [rsp + 0x110]
0x1406DC742: movaps    xmm6, xmmword ptr [rsp + 0xc0]
0x1406DC74A: add       rsp, 0xd0
0x1406DC751: pop       r15
0x1406DC753: pop       r14
0x1406DC755: pop       r13
0x1406DC757: pop       r12
0x1406DC759: pop       rdi
0x1406DC75A: pop       rsi
0x1406DC75B: pop       rbp
0x1406DC75C: ret       
0x1406DC75D: int3      
0x1406DC75E: int3      
0x1406DC75F: int3      
0x1406DC760: int3      
0x1406DC761: int3      
0x1406DC762: int3      
0x1406DC763: int3      
0x1406DC764: int3      
0x1406DC765: int3      
0x1406DC766: int3      
0x1406DC767: int3      
0x1406DC768: int3      
0x1406DC769: int3      
0x1406DC76A: int3      
0x1406DC76B: int3      
0x1406DC76C: int3      
0x1406DC76D: int3      
0x1406DC76E: int3      
0x1406DC76F: int3      
0x1406DC770: mov       r11, rsp
0x1406DC773: push      rbp
0x1406DC774: push      rsi
0x1406DC775: push      rdi
0x1406DC776: push      r12
0x1406DC778: push      r13
0x1406DC77A: push      r14
0x1406DC77C: push      r15
0x1406DC77E: sub       rsp, 0x50
0x1406DC782: mov       qword ptr [r11 - 0x68], 0xfffffffffffffffe
0x1406DC78A: mov       qword ptr [r11 + 0x20], rbx
0x1406DC78E: mov       r12, rdx
0x1406DC791: mov       rsi, rcx
0x1406DC794: mov       r8d, dword ptr [rip + 0x2f140dd]
0x1406DC79B: mov       rax, qword ptr gs:[0x58]
0x1406DC7A4: mov       r13d, 0x768
0x1406DC7AA: mov       rax, qword ptr [rax + r8*8]
0x1406DC7AE: add       r13, rax
0x1406DC7B1: mov       qword ptr [rsp + 0xa0], r13
0x1406DC7B9: mov       ebx, dword ptr [r13]
0x1406DC7BD: mov       dword ptr [r11 + 0x10], ebx
0x1406DC7C1: mov       dword ptr [r13], 0x72
0x1406DC7C9: test      rcx, rcx
0x1406DC7CC: je        0x1406dca84
0x1406DC7D2: mov       rax, qword ptr [rdx]
0x1406DC7D5: cmp       rcx, rax
0x1406DC7D8: je        0x1406dca84
0x1406DC7DE: test      dword ptr [rcx + 0x10], 0x4820
0x1406DC7E5: jne       0x1406dca84
0x1406DC7EB: test      rax, rax
0x1406DC7EE: je        0x1406dc80e
0x1406DC7F0: xor       dil, dil
0x1406DC7F3: xor       r14d, r14d
0x1406DC7F6: cmp       byte ptr [rcx + 0x1a], 0x3e
0x1406DC7FA: cmove     r14, rcx
0x1406DC7FE: mov       rbp, qword ptr [rcx + 0x40]
0x1406DC802: movzx     r15d, byte ptr [rbp + 0x1a]
0x1406DC807: cmp       qword ptr [rdx + 8], 0
0x1406DC80C: jne       0x1406dc815
0x1406DC80E: mov       al, 1
0x1406DC810: jmp       0x1406dca86   ; ->VA 0x1406DCA86 (file 0x6DBE86)
0x1406DC815: mov       eax, dword ptr [rdx + 0x10]
0x1406DC818: mov       edx, 1
0x1406DC81D: test      eax, eax
0x1406DC81F: je        0x1406dc860
0x1406DC821: mov       edx, eax
0x1406DC823: mov       rcx, rbp
0x1406DC826: call      0x140496780   ; ->VA 0x140496780 (file 0x495B80)
0x1406DC82B: test      al, al
0x1406DC82D: je        0x1406dc856
0x1406DC82F: xor       ecx, ecx
0x1406DC831: cmp       byte ptr [rsi + 0x1a], 0x3e
0x1406DC835: cmove     rcx, rsi
0x1406DC839: mov       rdx, qword ptr [r12 + 0x20]
0x1406DC83E: test      rdx, rdx
0x1406DC841: je        0x1406dc889
0x1406DC843: test      rcx, rcx
0x1406DC846: je        0x1406dc889
0x1406DC848: mov       rax, qword ptr [rcx]
0x1406DC84B: call      qword ptr [rax + 0x7c8]
0x1406DC851: movzx     edi, al
0x1406DC854: jmp       0x1406dc897   ; ->VA 0x1406DC897 (file 0x6DBC97)
0x1406DC856: mov       eax, dword ptr [r12 + 0x10]
0x1406DC85B: mov       edx, 1
0x1406DC860: mov       rcx, qword ptr [r12 + 0x18]
0x1406DC865: test      rcx, rcx
0x1406DC868: je        0x1406dc88e
0x1406DC86A: cmp       rcx, rbp
0x1406DC86D: jne       0x1406dc874
0x1406DC86F: mov       dil, 1
0x1406DC872: jmp       0x1406dc897   ; ->VA 0x1406DC897 (file 0x6DBC97)
0x1406DC874: test      r14, r14
0x1406DC877: je        0x1406dc897
0x1406DC879: lea       rcx, [rsi + 0x70]
0x1406DC87D: call      0x1401640e0   ; ->VA 0x1401640E0 (file 0x1634E0)
0x1406DC882: cmp       rax, qword ptr [r12 + 0x18]
0x1406DC887: jne       0x1406dc897
0x1406DC889: mov       dil, 1
0x1406DC88C: jmp       0x1406dc897   ; ->VA 0x1406DC897 (file 0x6DBC97)
0x1406DC88E: movzx     edi, dil
0x1406DC892: test      eax, eax
0x1406DC894: cmove     edi, edx
0x1406DC897: cmp       r15b, 0x1b
0x1406DC89B: je        0x1406dc8be
0x1406DC89D: cmp       r15b, 0x1f
0x1406DC8A1: jne       0x1406dc8c7
0x1406DC8A3: mov       eax, dword ptr [rbp + 0xec]
0x1406DC8A9: shr       eax, 1
0x1406DC8AB: test      al, 1
0x1406DC8AD: mov       ecx, 0
0x1406DC8B2: movzx     eax, dil
0x1406DC8B6: cmovne    ecx, eax
0x1406DC8B9: movzx     edi, cl
0x1406DC8BC: jmp       0x1406dc8c7   ; ->VA 0x1406DC8C7 (file 0x6DBCC7)
0x1406DC8BE: test      byte ptr [rbp + 0x110], 2
0x1406DC8C5: jne       0x1406dc93f
0x1406DC8C7: test      dil, dil
0x1406DC8CA: je        0x1406dc93f
0x1406DC8CC: mov       dword ptr [rsp + 0x3c], 0x7fffffff
0x1406DC8D4: mov       word ptr [rsp + 0x4c], 0
0x1406DC8DB: mov       rax, qword ptr [rsi + 0x40]
0x1406DC8DF: mov       qword ptr [rsp + 0x30], rax
0x1406DC8E4: test      dword ptr [rsi + 0x28], 0x3ff
0x1406DC8EB: jne       0x1406dc8f5
0x1406DC8ED: mov       eax, dword ptr [rip + 0x1a1a0a9]
0x1406DC8F3: jmp       0x1406dc90c   ; ->VA 0x1406DC90C (file 0x6DBD0C)
0x1406DC8F5: mov       rdx, rsi
0x1406DC8F8: lea       rcx, [rsp + 0x90]
0x1406DC900: call      0x140179050   ; ->VA 0x140179050 (file 0x178450)
0x1406DC905: mov       eax, dword ptr [rsp + 0x90]
0x1406DC90C: mov       dword ptr [rsp + 0x28], eax
0x1406DC910: lea       rdi, [rsi + 0x70]
0x1406DC914: mov       rcx, rdi
0x1406DC917: call      0x14015f360   ; ->VA 0x14015F360 (file 0x15E760)
0x1406DC91C: movsx     ecx, ax
0x1406DC91F: mov       dword ptr [rsp + 0x38], ecx
0x1406DC923: mov       dword ptr [rsp + 0x48], 1
0x1406DC92B: mov       qword ptr [rsp + 0x40], rdi
0x1406DC930: lea       rcx, [r12 + 0x28]
0x1406DC935: lea       rdx, [rsp + 0x28]
0x1406DC93A: call      0x1406dd130   ; ->VA 0x1406DD130 (file 0x6DC530)
0x1406DC93F: movsxd    rax, dword ptr [r12 + 0x10]
0x1406DC944: cmp       eax, 0x14
0x1406DC947: ja        0x1406dc972
0x1406DC949: lea       rdx, [rip - 0x6dc950]
0x1406DC950: movzx     eax, byte ptr [rdx + rax + 0x6dcaac]
0x1406DC958: mov       ecx, dword ptr [rdx + rax*4 + 0x6dcaa4]
0x1406DC95F: add       rcx, rdx
0x1406DC962: jmp       rcx
0x1406DC964: test      r14, r14
0x1406DC967: je        0x1406dc972
0x1406DC969: cmp       rsi, qword ptr [rip + 0x2aaab88]
0x1406DC970: jne       0x1406dc97c
0x1406DC972: cmp       r15b, 0x1c
0x1406DC976: jne       0x1406dca84
0x1406DC97C: lea       rcx, [rsi + 0x70]
0x1406DC980: call      0x14015f690   ; ->VA 0x14015F690 (file 0x15EA90)
0x1406DC985: mov       r13, rax
0x1406DC988: test      rax, rax
0x1406DC98B: je        0x1406dca02
0x1406DC98D: mov       rcx, rax
0x1406DC990: call      0x14022b7c0   ; ->VA 0x14022B7C0 (file 0x22ABC0)
0x1406DC995: test      rax, rax
0x1406DC998: je        0x1406dca02
0x1406DC99A: mov       rcx, r13
0x1406DC99D: call      0x14022b7c0   ; ->VA 0x14022B7C0 (file 0x22ABC0)
0x1406DC9A2: mov       ecx, dword ptr [rax + 0x10]
0x1406DC9A5: mov       r14, qword ptr [rax + 8]
0x1406DC9A9: test      ecx, ecx
0x1406DC9AB: je        0x1406dca02
0x1406DC9AD: mov       r15d, ecx
0x1406DC9B0: mov       rdi, qword ptr [r14]
0x1406DC9B3: test      rdi, rdi
0x1406DC9B6: je        0x1406dc9f8
0x1406DC9B8: xor       r8d, r8d
0x1406DC9BB: mov       rdx, qword ptr [rdi + 8]
0x1406DC9BF: mov       rcx, r13
0x1406DC9C2: call      0x140237310   ; ->VA 0x140237310 (file 0x236710)
0x1406DC9C7: mov       rbp, rax
0x1406DC9CA: test      rax, rax
0x1406DC9CD: jne       0x1406dc9e3
0x1406DC9CF: xor       r9d, r9d
0x1406DC9D2: mov       r8, rsi
0x1406DC9D5: mov       rdx, qword ptr [rdi + 8]
0x1406DC9D9: mov       rcx, r12
0x1406DC9DC: call      0x1406dd1d0   ; ->VA 0x1406DD1D0 (file 0x6DC5D0)
0x1406DC9E1: jmp       0x1406dc9f8   ; ->VA 0x1406DC9F8 (file 0x6DBDF8)
0x1406DC9E3: mov       rcx, rbp
0x1406DC9E6: call      0x140222820   ; ->VA 0x140222820 (file 0x221C20)
0x1406DC9EB: mov       edx, 0x18
0x1406DC9F0: mov       rcx, rbp
0x1406DC9F3: call      0x14153ab8c   ; ->VA 0x14153AB8C (file 0x1539F8C)
0x1406DC9F8: add       r14, 8
0x1406DC9FC: sub       r15, 1
0x1406DCA00: jne       0x1406dc9b0
0x1406DCA02: mov       r8b, 1
0x1406DCA05: xor       edx, edx
0x1406DCA07: mov       rcx, rsi
0x1406DCA0A: call      0x1402e1fb0   ; ->VA 0x1402E1FB0 (file 0x2E13B0)
0x1406DCA0F: mov       r14d, eax
0x1406DCA12: xor       ebp, ebp
0x1406DCA14: test      eax, eax
0x1406DCA16: jle       0x1406dca7c
0x1406DCA18: nop       dword ptr [rax + rax]
0x1406DCA20: xor       r8d, r8d
0x1406DCA23: mov       edx, ebp
0x1406DCA25: mov       rcx, rsi
0x1406DCA28: call      0x1402e1f40   ; ->VA 0x1402E1F40 (file 0x2E1340)
0x1406DCA2D: mov       rdi, rax
0x1406DCA30: test      rax, rax
0x1406DCA33: je        0x1406dca75
0x1406DCA35: mov       rcx, rax
0x1406DCA38: call      0x140223440   ; ->VA 0x140223440 (file 0x222840)
0x1406DCA3D: test      al, al
0x1406DCA3F: jne       0x1406dca60
0x1406DCA41: mov       rax, qword ptr [rdi + 8]
0x1406DCA45: test      rax, rax
0x1406DCA48: je        0x1406dca4f
0x1406DCA4A: mov       r9, qword ptr [rax]
0x1406DCA4D: jmp       0x1406dca52   ; ->VA 0x1406DCA52 (file 0x6DBE52)
0x1406DCA4F: xor       r9d, r9d
0x1406DCA52: mov       r8, rsi
0x1406DCA55: mov       rdx, qword ptr [rdi]
0x1406DCA58: mov       rcx, r12
0x1406DCA5B: call      0x1406dd1d0   ; ->VA 0x1406DD1D0 (file 0x6DC5D0)
0x1406DCA60: mov       rcx, rdi
0x1406DCA63: call      0x140222820   ; ->VA 0x140222820 (file 0x221C20)
0x1406DCA68: mov       edx, 0x18
0x1406DCA6D: mov       rcx, rdi
0x1406DCA70: call      0x14153ab8c   ; ->VA 0x14153AB8C (file 0x1539F8C)
0x1406DCA75: inc       ebp
0x1406DCA77: cmp       ebp, r14d
0x1406DCA7A: jl        0x1406dca20
0x1406DCA7C: mov       r13, qword ptr [rsp + 0xa0]
0x1406DCA84: xor       al, al
0x1406DCA86: mov       dword ptr [r13], ebx
0x1406DCA8A: mov       rbx, qword ptr [rsp + 0xa8]
0x1406DCA92: add       rsp, 0x50
0x1406DCA96: pop       r15
0x1406DCA98: pop       r14
0x1406DCA9A: pop       r13
0x1406DCA9C: pop       r12
0x1406DCA9E: pop       rdi
0x1406DCA9F: pop       rsi
0x1406DCAA0: pop       rbp
0x1406DCAA1: ret       
0x1406DCAA2: nop       
0x1406DCAA4: leave     
0x1406DCAA6: insd      dword ptr [rdi], dx
0x1406DCAA7: add       byte ptr [rdx - 0x37], dh
0x1406DCAAA: insd      dword ptr [rdi], dx
0x1406DCAAB: add       byte ptr [rax], al
0x1406DCAAD: add       dword ptr [rax], eax
0x1406DCAAF: add       byte ptr [rcx], al
0x1406DCAB1: add       dword ptr [rax], eax
0x1406DCAB3: add       byte ptr [rax], al
0x1406DCAB5: add       byte ptr [rcx], al
0x1406DCAB7: add       byte ptr [rax], al
0x1406DCAB9: add       byte ptr [rax], al
0x1406DCABB: add       byte ptr [rax], al
0x1406DCABD: add       byte ptr [rcx], al
0x1406DCABF: add       byte ptr [rax], al
0x1406DCAC1: int3      
0x1406DCAC2: int3      
0x1406DCAC3: int3      
0x1406DCAC4: int3      
0x1406DCAC5: int3      
0x1406DCAC6: int3      
0x1406DCAC7: int3      
0x1406DCAC8: int3      
0x1406DCAC9: int3      
0x1406DCACA: int3      
0x1406DCACB: int3      
0x1406DCACC: int3      
0x1406DCACD: int3      
0x1406DCACE: int3      
0x1406DCACF: int3      
0x1406DCAD0: mov       qword ptr [rsp + 8], rcx
0x1406DCAD5: push      rbx
0x1406DCAD6: push      rsi
0x1406DCAD7: push      rdi
0x1406DCAD8: sub       rsp, 0x20
0x1406DCADC: mov       rbx, rdx
0x1406DCADF: mov       rsi, rcx
0x1406DCAE2: test      rdx, rdx
0x1406DCAE5: je        0x1406dcc8e
0x1406DCAEB: mov       rdi, qword ptr [rdx]
0x1406DCAEE: test      rcx, rcx
0x1406DCAF1: je        0x1406dcc84
0x1406DCAF7: mov       ecx, dword ptr [rcx + 0x10]
0x1406DCAFA: mov       eax, ecx
0x1406DCAFC: shr       eax, 5
0x1406DCAFF: test      al, 1
0x1406DCB01: jne       0x1406dcc84
0x1406DCB07: mov       eax, ecx
0x1406DCB09: shr       eax, 0xe
0x1406DCB0C: test      al, 1
0x1406DCB0E: jne       0x1406dcc84
0x1406DCB14: shr       ecx, 0xb
0x1406DCB17: test      cl, 1
0x1406DCB1A: jne       0x1406dcc84
0x1406DCB20: mov       rax, qword ptr [rdi]
0x1406DCB23: mov       rcx, rdi
0x1406DCB26: call      qword ptr [rax + 0x2f0]
0x1406DCB2C: test      al, al
0x1406DCB2E: je        0x1406dcb43
0x1406DCB30: mov       rdx, rsi
0x1406DCB33: mov       rcx, rdi
0x1406DCB36: call      0x14068d8a0   ; ->VA 0x14068D8A0 (file 0x68CCA0)
0x1406DCB3B: test      al, al
0x1406DCB3D: je        0x1406dcc84
0x1406DCB43: cmp       qword ptr [rdi + 0xf8], 0
0x1406DCB4B: je        0x1406dcc8e
0x1406DCB51: mov       rcx, qword ptr [rsi + 0x40]
0x1406DCB55: mov       edx, dword ptr [rbx + 8]
0x1406DCB58: mov       qword ptr [rsp + 0x48], rbp
0x1406DCB5D: xor       ebp, ebp
0x1406DCB5F: cmp       byte ptr [rsi + 0x1a], 0x3e
0x1406DCB63: mov       edi, ebp
0x1406DCB65: mov       qword ptr [rsp + 0x50], r14
0x1406DCB6A: movzx     r14d, byte ptr [rcx + 0x1a]
0x1406DCB6F: cmove     rdi, rsi
0x1406DCB73: test      edx, edx
0x1406DCB75: je        0x1406dcb92
0x1406DCB77: call      0x140496780   ; ->VA 0x140496780 (file 0x495B80)
0x1406DCB7C: test      al, al
0x1406DCB7E: je        0x1406dcb92
0x1406DCB80: mov       rax, qword ptr [rsi]
0x1406DCB83: xor       edx, edx
0x1406DCB85: mov       rcx, rsi
0x1406DCB88: call      qword ptr [rax + 0x4c8]
0x1406DCB8E: test      al, al
0x1406DCB90: jmp       0x1406dcba9   ; ->VA 0x1406DCBA9 (file 0x6DBFA9)
0x1406DCB92: mov       rax, qword ptr [rbx + 0x30]
0x1406DCB96: test      rax, rax
0x1406DCB99: je        0x1406dcba1
0x1406DCB9B: cmp       rax, qword ptr [rsi + 0x40]
0x1406DCB9F: je        0x1406dcb80
0x1406DCBA1: cmp       dword ptr [rbx + 8], ebp
0x1406DCBA4: jne       0x1406dcbc3
0x1406DCBA6: test      rax, rax
0x1406DCBA9: jne       0x1406dcbc3
0x1406DCBAB: mov       dl, 1
0x1406DCBAD: mov       rcx, rsi
0x1406DCBB0: call      0x1402de3b0   ; ->VA 0x1402DE3B0 (file 0x2DD7B0)
0x1406DCBB5: lea       rdx, [rsp + 0x40]
0x1406DCBBA: lea       rcx, [rbx + 0x10]
0x1406DCBBE: call      0x1406dd0c0   ; ->VA 0x1406DD0C0 (file 0x6DC4C0)
0x1406DCBC3: test      rdi, rdi
0x1406DCBC6: je        0x1406dcbd1
0x1406DCBC8: cmp       rsi, qword ptr [rip + 0x2aaa929]
0x1406DCBCF: jne       0x1406dcbdb
0x1406DCBD1: cmp       r14b, 0x1c
0x1406DCBD5: jne       0x1406dcc7a
0x1406DCBDB: mov       r8b, 1
0x1406DCBDE: xor       edx, edx
0x1406DCBE0: mov       rcx, rsi
0x1406DCBE3: call      0x1402e1fb0   ; ->VA 0x1402E1FB0 (file 0x2E13B0)
0x1406DCBE8: mov       r14d, eax
0x1406DCBEB: test      eax, eax
0x1406DCBED: jle       0x1406dcc7a
0x1406DCBF3: xor       r8d, r8d
0x1406DCBF6: mov       edx, ebp
0x1406DCBF8: mov       rcx, rsi
0x1406DCBFB: call      0x1402e1f40   ; ->VA 0x1402E1F40 (file 0x2E1340)
0x1406DCC00: mov       rdi, rax
0x1406DCC03: test      rax, rax
0x1406DCC06: je        0x1406dcc6f
0x1406DCC08: mov       rcx, rax
0x1406DCC0B: call      0x140223440   ; ->VA 0x140223440 (file 0x222840)
0x1406DCC10: test      al, al
0x1406DCC12: jne       0x1406dcc5a
0x1406DCC14: mov       edx, dword ptr [rbx + 8]
0x1406DCC17: test      edx, edx
0x1406DCC19: je        0x1406dcc27
0x1406DCC1B: mov       rcx, qword ptr [rdi]
0x1406DCC1E: call      0x140496780   ; ->VA 0x140496780 (file 0x495B80)
0x1406DCC23: test      al, al
0x1406DCC25: jne       0x1406dcc42
0x1406DCC27: mov       rax, qword ptr [rbx + 0x30]
0x1406DCC2B: cmp       rax, qword ptr [rdi]
0x1406DCC2E: je        0x1406dcc42
0x1406DCC30: cmp       dword ptr [rbx + 8], 0
0x1406DCC34: jne       0x1406dcc5a
0x1406DCC36: cmp       qword ptr [rbx + 0x38], 0
0x1406DCC3B: jne       0x1406dcc5a
0x1406DCC3D: test      rax, rax
0x1406DCC40: jne       0x1406dcc5a
0x1406DCC42: mov       dl, 1
0x1406DCC44: mov       rcx, rsi
0x1406DCC47: call      0x1402de3b0   ; ->VA 0x1402DE3B0 (file 0x2DD7B0)
0x1406DCC4C: lea       rcx, [rbx + 0x10]
0x1406DCC50: lea       rdx, [rsp + 0x40]
0x1406DCC55: call      0x1406dd0c0   ; ->VA 0x1406DD0C0 (file 0x6DC4C0)
0x1406DCC5A: mov       rcx, rdi
0x1406DCC5D: call      0x140222820   ; ->VA 0x140222820 (file 0x221C20)
0x1406DCC62: mov       edx, 0x18
0x1406DCC67: mov       rcx, rdi
0x1406DCC6A: call      0x14153ab8c   ; ->VA 0x14153AB8C (file 0x1539F8C)
0x1406DCC6F: inc       ebp
0x1406DCC71: cmp       ebp, r14d
0x1406DCC74: jl        0x1406dcbf3
0x1406DCC7A: mov       rbp, qword ptr [rsp + 0x48]
0x1406DCC7F: mov       r14, qword ptr [rsp + 0x50]
0x1406DCC84: xor       al, al
0x1406DCC86: add       rsp, 0x20
0x1406DCC8A: pop       rdi
0x1406DCC8B: pop       rsi
0x1406DCC8C: pop       rbx
0x1406DCC8D: ret       
0x1406DCC8E: mov       al, 1
0x1406DCC90: add       rsp, 0x20
0x1406DCC94: pop       rdi
0x1406DCC95: pop       rsi
0x1406DCC96: pop       rbx
0x1406DCC97: ret       
0x1406DCC98: int3      
0x1406DCC99: int3      
0x1406DCC9A: int3      
0x1406DCC9B: int3      
0x1406DCC9C: int3      
0x1406DCC9D: int3      
0x1406DCC9E: int3      
0x1406DCC9F: int3      
0x1406DCCA0: mov       qword ptr [rsp + 0x10], rbx
0x1406DCCA5: mov       qword ptr [rsp + 0x18], rbp
0x1406DCCAA: push      rsi
0x1406DCCAB: push      rdi
0x1406DCCAC: push      r12
0x1406DCCAE: push      r13
0x1406DCCB0: push      r15
0x1406DCCB2: sub       rsp, 0x20
0x1406DCCB6: mov       r12, qword ptr [rcx + 8]
0x1406DCCBA: mov       ebx, r9d
0x1406DCCBD: mov       edi, r8d
0x1406DCCC0: mov       r15, rdx
0x1406DCCC3: mov       esi, r8d
0x1406DCCC6: mov       rbp, rcx
0x1406DCCC9: lea       r13, [rdi*8]
0x1406DCCD1: cmp       r8d, r9d
0x1406DCCD4: jae       0x1406dcd05
0x1406DCCD6: mov       qword ptr [rsp + 0x50], r14
0x1406DCCDB: nop       dword ptr [rax + rax]
0x1406DCCE0: mov       rcx, qword ptr [rbp + 8]
0x1406DCCE4: mov       rdx, qword ptr [r12 + r13]
0x1406DCCE8: mov       r14d, ebx
0x1406DCCEB: mov       rcx, qword ptr [rcx + r14*8]
0x1406DCCEF: call      r15
0x1406DCCF2: test      eax, eax
0x1406DCCF4: jle       0x1406dcd33
0x1406DCCF6: dec       ebx
0x1406DCCF8: cmp       edi, ebx
0x1406DCCFA: jb        0x1406dcce0
0x1406DCCFC: mov       r12, qword ptr [rbp + 8]
0x1406DCD00: mov       r14, qword ptr [rsp + 0x50]
0x1406DCD05: mov       rcx, qword ptr [r12 + r13]
0x1406DCD09: mov       rbp, qword ptr [rsp + 0x60]
0x1406DCD0E: mov       eax, ebx
0x1406DCD10: lea       rdx, [r12 + rax*8]
0x1406DCD14: mov       rax, qword ptr [r12 + rax*8]
0x1406DCD18: mov       qword ptr [r12 + r13], rax
0x1406DCD1C: mov       eax, ebx
0x1406DCD1E: mov       rbx, qword ptr [rsp + 0x58]
0x1406DCD23: mov       qword ptr [rdx], rcx
0x1406DCD26: add       rsp, 0x20
0x1406DCD2A: pop       r15
0x1406DCD2C: pop       r13
0x1406DCD2E: pop       r12
0x1406DCD30: pop       rdi
0x1406DCD31: pop       rsi
0x1406DCD32: ret       
0x1406DCD33: lea       rsi, [rsi*8]
0x1406DCD3B: nop       dword ptr [rax + rax]
0x1406DCD40: mov       rcx, qword ptr [rbp + 8]
0x1406DCD44: mov       rdx, qword ptr [r12 + r13]
0x1406DCD48: mov       rcx, qword ptr [rsi + rcx]
0x1406DCD4C: call      r15
0x1406DCD4F: test      eax, eax
0x1406DCD51: jg        0x1406dcd5f
0x1406DCD53: inc       edi
0x1406DCD55: add       rsi, 8
0x1406DCD59: cmp       edi, ebx
0x1406DCD5B: jb        0x1406dcd40
0x1406DCD5D: jmp       0x1406dccfc   ; ->VA 0x1406DCCFC (file 0x6DC0FC)
0x1406DCD5F: mov       rax, qword ptr [rbp + 8]
0x1406DCD63: mov       rcx, qword ptr [rax + rdi*8]
0x1406DCD67: lea       rdx, [rax + rdi*8]
0x1406DCD6B: mov       esi, edi
0x1406DCD6D: lea       r8, [rax + r14*8]
0x1406DCD71: mov       rax, qword ptr [rax + r14*8]
0x1406DCD75: mov       qword ptr [rdx], rax
0x1406DCD78: mov       qword ptr [r8], rcx
0x1406DCD7B: jmp       0x1406dcce0   ; ->VA 0x1406DCCE0 (file 0x6DC0E0)
0x1406DCD80: cmp       r8d, r9d
0x1406DCD83: jae       0x1406dce79
0x1406DCD89: mov       dword ptr [rsp + 0x20], r9d
0x1406DCD8E: push      rbp
0x1406DCD8F: push      r12
0x1406DCD91: push      r13
0x1406DCD93: push      r14
0x1406DCD95: sub       rsp, 0x38
0x1406DCD99: mov       qword ptr [rsp + 0x60], rbx
0x1406DCD9E: mov       r14d, r9d
0x1406DCDA1: mov       qword ptr [rsp + 0x68], rsi
0x1406DCDA6: mov       r13d, r8d
0x1406DCDA9: mov       qword ptr [rsp + 0x70], rdi
0x1406DCDAE: mov       r12, rdx
0x1406DCDB1: mov       qword ptr [rsp + 0x30], r15
0x1406DCDB6: mov       rbp, rcx
0x1406DCDB9: nop       dword ptr [rax]
0x1406DCDC0: mov       r8, qword ptr [rbp + 8]
0x1406DCDC4: mov       edi, r13d
0x1406DCDC7: mov       esi, r13d
0x1406DCDCA: mov       ebx, r14d
0x1406DCDCD: lea       r9, [rsi*8]
0x1406DCDD5: mov       qword ptr [rsp + 0x20], r9
0x1406DCDDA: lea       r15, [r9 + r8]
0x1406DCDDE: cmp       r13d, r14d
0x1406DCDE1: jae       0x1406dce19
0x1406DCDE3: nop       dword ptr [rax]
0x1406DCDE7: nop       word ptr [rax + rax]
0x1406DCDF0: mov       rcx, qword ptr [rbp + 8]
0x1406DCDF4: mov       rdx, qword ptr [r15]
0x1406DCDF7: mov       r14d, ebx
0x1406DCDFA: mov       rcx, qword ptr [rcx + r14*8]
0x1406DCDFE: call      r12
0x1406DCE01: test      eax, eax
0x1406DCE03: jle       0x1406dce7a
0x1406DCE05: dec       ebx
