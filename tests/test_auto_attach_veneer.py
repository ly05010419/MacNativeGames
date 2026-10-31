"""Offline ABI probe of our own veneer, with game routines replaced by callbacks. No game content is loaded."""
import re
import struct
import unittest
from pathlib import Path

try:
    import unicorn as uc
    from unicorn import arm64_const as a64
except ImportError:
    uc = None

SOURCE = Path(__file__).resolve().parents[1] / 'games/totk/runtime/auto_attach.c'
BASE, STUB, DATA, STACK, STOP = 0x10000000, 0x144F27C0, 0x30000000, 0x40000000, 0x50000000
DECIDE, MANUAL, CONTROL, GOT, SLOT = DATA, DATA+0x100, DATA+0x200, DATA+0x300, DATA+0x308
POLL, REQUEST, STOCK, REMOVE = [BASE+n for n in (0x1B64A9C, 0x1B64BB0, 0xD62B58, 0x1D1FE48)]


@unittest.skipUnless(uc, 'offline veneer probe needs Unicorn; never used in the runtime')
class AutoAttachVeneerTests(unittest.TestCase):
    def execute(self, *, on=1, decision=0, stock=1, manual=False):
        source=SOURCE.read_text()
        block=source.split('static const uint32_t veneer[]={',1)[1].split('};',1)[0]
        words=[int(w,16) for w in re.findall(r'0x([0-9a-f]+)u',block)]
        for index,off,target,call in re.findall(r'words\[(\d+)\]=jump\(stub\+(\d+),0x([0-9a-f]+),([01])\)',source):
            delta=BASE+int(target,16)-(STUB+int(off))
            words[int(index)]=(0x94000000 if call=='1' else 0x14000000)|((delta//4)&0x3ffffff)
        code=bytearray(struct.pack(f'<{len(words)}I',*words))
        for off,value in ((200,CONTROL),(208,DECIDE),(216,GOT),(272,CONTROL),(280,MANUAL)):
            struct.pack_into('<Q',code,off,value)
        engine=uc.Uc(uc.UC_ARCH_ARM64,uc.UC_MODE_ARM)
        for page in {v&~0xFFF for v in (STUB,DATA,STACK,STOP,POLL,REQUEST,STOCK,REMOVE)}:
            engine.mem_map(page,0x1000)
        engine.mem_write(STUB,bytes(code));engine.mem_write(CONTROL,struct.pack('<I',on))
        engine.mem_write(GOT,struct.pack('<QQ',SLOT,0x12345678))
        registers=[getattr(a64,f'UC_ARM64_REG_X{i}') for i in range(31)]
        original={registers[i]:0x60000000+i*0x100 for i in range(29)}
        for register,value in original.items():engine.reg_write(register,value)
        engine.reg_write(a64.UC_ARM64_REG_SP,STACK+0xF00)
        engine.reg_write(a64.UC_ARM64_REG_X30,STOP)
        events=[]
        targets={DECIDE:'decide',MANUAL:'manual',POLL:'poll',REQUEST:'request',STOCK:'stock',REMOVE:'remove'}
        def hit(m,address,size,data):
            if address==STOP:m.emu_stop();return
            if address not in targets:return
            name=targets[address];args=tuple(m.reg_read(r) for r in registers[:5]);events.append((name,args))
            for i in range(19):m.reg_write(registers[i],0xBAD00000+i)
            value=decision if name=='decide' else stock if name=='stock' else 0xABCD
            m.reg_write(a64.UC_ARM64_REG_X0,value)
            m.reg_write(a64.UC_ARM64_REG_PC,m.reg_read(a64.UC_ARM64_REG_X30))
        engine.hook_add(uc.UC_HOOK_CODE,hit)
        engine.emu_start(STUB+(224 if manual else 0),STOP+4,count=1000)
        self.assertEqual(engine.reg_read(a64.UC_ARM64_REG_PC),STOP)
        self.assertEqual(engine.reg_read(a64.UC_ARM64_REG_SP),STACK+0xF00)
        for i in range(19,29):self.assertEqual(engine.reg_read(registers[i]),original[registers[i]],f'x{i}')
        self.assertEqual(engine.reg_read(a64.UC_ARM64_REG_X0),0xABCD)
        normal=next(args for name,args in events if name==('request' if manual else 'poll'))
        self.assertEqual(normal[:2],(original[registers[0]],original[registers[1]]))
        if not manual:self.assertEqual(normal[2],original[registers[2]])
        return events

    def test_off_is_original_call_only(self):
        self.assertEqual([n for n,a in self.execute(on=0)],['poll'])

    def test_manual_disabled_is_original_call_only(self):
        self.assertEqual([n for n,a in self.execute(on=0,manual=True)],['request'])

    def test_no_action_keeps_original_poll(self):
        self.assertEqual([n for n,a in self.execute()],['decide','poll'])

    def test_no_stock_does_not_request_material(self):
        self.assertEqual([n for n,a in self.execute(decision=0x60000100,stock=0)],['decide','stock','poll'])

    def test_stock_checked_before_request(self):
        events=self.execute(decision=0x60000100)
        self.assertEqual([n for n,a in events],['decide','stock','request','poll'])
        self.assertEqual(events[1][1],(0x12345678,0x60000100,1,0,1))
        self.assertEqual(events[2][1][:2],(0x60000000,0x60000100))

    def test_remove_does_not_request_or_consume(self):
        events=self.execute(decision=1)
        self.assertEqual([n for n,a in events],['decide','remove','poll'])
        self.assertEqual(events[1][1][0],0x60001300)

    def test_manual_records_then_forwards_original_arguments(self):
        events=self.execute(manual=True)
        self.assertEqual([n for n,a in events],['manual','request'])
        self.assertEqual(events[0][1][:2],(0x60001300,0x60000100))
