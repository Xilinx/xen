DefinitionBlock ("DSDT.aml", "DSDT", 5, "Xen", "PVH", 0)
{
    Scope ( \_SB )
    {
        /* Reserve PCI Segment 1 Root Bridge ECAM */
        Device (RES1)
        {
            Name (_HID, EISAID("PNP0C02"))
            Method (_STA, 0, NotSerialized)
            {
                If(LEqual(\_SB.ECA1, 0)) {
                    Return(0x00)
                } Else {
                    Return(0x0F)
                }
            }
            Method (_CRS, 0, NotSerialized)
            {
                Store (ResourceTemplate ()
                {
                    DWordMemory (
                        ResourceProducer, PosDecode, MinFixed, MaxFixed,
                        NonCacheable, ReadWrite,
                        0x0, /* _GRA */
                        0x0, /* _MIN */
                        0x0, /* _MAX */
                        0x0, /* _TRA */
                        0x0, /* _LEN */
                        ,, _Y01)
                 }, Local1)

                 CreateDWordField(Local1, \_SB.RES1._CRS._Y01._MIN, MMIN)
                 CreateDWordField(Local1, \_SB.RES1._CRS._Y01._MAX, MMAX)
                 CreateDWordField(Local1, \_SB.RES1._CRS._Y01._LEN, MLEN)

                 /* If PCI Segment 1 Root Bridge is not enabled, expose
                    a zero-length resource to be ignored */
                 If(LEqual(\_SB.ECA1, Zero)) {
                     Store (Zero, MMIN)
                     Store (Zero, MMAX)
                     Store (Zero, MLEN)
                 } Else {
                     Store(\_SB.ECA1, MMIN)
                     Store(\_SB.MXB1, MLEN)
                     Add(MLEN, One, MLEN)
                     Multiply(MLEN, 0x100000, MLEN)
                     Subtract(MLEN, One, MMAX)
                     Add(MMAX, MMIN, MMAX)
                 }

                 Return(Local1)
             }
        }

        /* PCI Segment 1 Root Bridge */
        Device (PCI1)
        {
            Name (_HID, EISAID("PNP0A08"))
            Name (_CID, EISAID("PNP0A03"))
            Name (_SEG, 1)
            Name (_BBN, 0)
            Name (_CCA, 1) /*_CCA: Cache Coherency Attribute */
            Method (_STA, 0, NotSerialized)
            {
                If(LEqual(\_SB.ECA1, 0)) {
                    Return(0x00)
                } Else {
                    Return(0x0F)
                }
            }
            Method (_CBA, 0, NotSerialized)
            {
                Return(\_SB.ECA1)
            }
            Method (_CRS, 0, NotSerialized)
            {
                Store (ResourceTemplate ()
                {
                    WordBusNumber (
                        ResourceProducer, MinFixed, MaxFixed, SubDecode,
                        0x0, /* _GRA */
                        0x0, /* _MIN */
                        0x0, /* _MAX */
                        0x0, /* _TRA */
                        0x0, /* _LEN */
                        ,, _Y01)
                    DWordMemory (
                        ResourceProducer, PosDecode, MinFixed, MaxFixed,
                        NonCacheable, ReadWrite,
                        0x0, /* _GRA */
                        0x0, /* _MIN */
                        0x0, /* _MAX */
                        0x0, /* _TRA */
                        0x0, /* _LEN */
                        ,, _Y02)
                    QWordMemory (
                        ResourceProducer, PosDecode, MinFixed, MaxFixed,
                        NonCacheable, ReadWrite,
                        0x0, /* _GRA */
                        0x0, /* _MIN */
                        0x0, /* _MAX */
                        0x0, /* _TRA */
                        0x0, /* _LEN */
                        ,, _Y03)
                }, Local1)

                CreateWordField(Local1, \_SB.PCI1._CRS._Y01._MAX, MAXB)
                CreateWordField(Local1, \_SB.PCI1._CRS._Y01._LEN, NUMB)
                Store(\_SB.MXB1, MAXB)
                Add(MAXB, One, NUMB)

                CreateDWordField(Local1, \_SB.PCI1._CRS._Y02._MIN, MMIN)
                CreateDWordField(Local1, \_SB.PCI1._CRS._Y02._MAX, MMAX)
                CreateDWordField(Local1, \_SB.PCI1._CRS._Y02._LEN, MLEN)
                Store(\_SB.PM1, MMIN)
                Store(\_SB.PL1, MLEN)
                Add(MMIN, MLEN, MMAX)
                Subtract(MMAX, One, MMAX)

                If(LEqual(Zero, \_SB.PCI1._CRS._Y03)) {
                    Subtract(\_SB.PCI1._CRS._Y03._MIN, 14, Local0)
                } Else {
                    Store(\_SB.PCI1._CRS._Y03, Local0)
                }
                CreateDWordField(Local1, Add(Local0, 14), MINL)
                CreateDWordField(Local1, Add(Local0, 18), MINH)
                CreateDWordField(Local1, Add(Local0, 22), MAXL)
                CreateDWordField(Local1, Add(Local0, 26), MAXH)
                CreateDWordField(Local1, Add(Local0, 38), LENL)
                CreateDWordField(Local1, Add(Local0, 42), LENH)

                Store(\_SB.PLM1, MINL)
                Store(\_SB.PHM1, MINH)
                Store(\_SB.PLL1, LENL)
                Store(\_SB.PHL1, LENH)
                Add(MINL, LENL, MAXL)
                Add(MINH, LENH, MAXH)
                If(LLess(MAXL, MINL)) {
                    Add(MAXH, One, MAXH)
                }
                If(LOr(MINH, LENL)) {
                    If(LEqual(MAXL, 0)) {
                        Subtract(MAXH, One, MAXH)
                    }
                    Subtract(MAXL, One, MAXL)
                }

                Return(Local1)
            }

            Device (GSI0)
            {
                Name (_HID, EISAID("PNP0C0F"))
                Name (_UID, 0)
                Method (_PRS, 0, NotSerialized)
                {
                    Store (ResourceTemplate ()
                    {
                        Interrupt (
                            ResourceConsumer, Level, ActiveHigh, Exclusive,
                            ,, _Y01)
                            { 0 } /* _INT */
                    }, Local1)

                    CreateDWordField(Local1, \_SB.PCI1.GSI0._PRS._Y01._INT, INT)
                    Store(\_SB.INT1, INT)
                    Return(Local1)
                }
                Method (_CRS, 0, NotSerialized)
                {
                    Store (ResourceTemplate ()
                    {
                        Interrupt (
                            ResourceConsumer, Level, ActiveHigh, Exclusive,
                            ,, _Y01)
                            { 0 } /* _INT */
                    }, Local1)

                    CreateDWordField(Local1, \_SB.PCI1.GSI0._CRS._Y01._INT, INT)
                    Store(\_SB.INT1, INT)
                    Return(Local1)
                }
                Method (_SRS, 1, NotSerialized)
                {
                }
            }

            Device (GSI1)
            {
                Name (_HID, EISAID("PNP0C0F"))
                Name (_UID, 1)
                Method (_PRS, 0, NotSerialized)
                {
                    Store (ResourceTemplate ()
                    {
                        Interrupt (
                            ResourceConsumer, Level, ActiveHigh, Exclusive,
                            ,, _Y01)
                            { 0 } /* _INT */
                    }, Local1)

                    CreateDWordField(Local1, \_SB.PCI1.GSI1._PRS._Y01._INT, INT)
                    Store(\_SB.INT1, INT)
                    Add(INT, 1, INT)
                    Return(Local1)
                }
                Method (_CRS, 0, NotSerialized)
                {
                    Store (ResourceTemplate ()
                    {
                        Interrupt (
                            ResourceConsumer, Level, ActiveHigh, Exclusive,
                            ,, _Y01)
                            { 0 } /* _INT */
                    }, Local1)

                    CreateDWordField(Local1, \_SB.PCI1.GSI1._CRS._Y01._INT, INT)
                    Store(\_SB.INT1, INT)
                    Add(INT, 1, INT)
                    Return(Local1)
                }
                Method (_SRS, 1, NotSerialized)
                {
                }
            }

            Device (GSI2)
            {
                Name (_HID, EISAID("PNP0C0F"))
                Name (_UID, 2)
                Method (_PRS, 0, NotSerialized)
                {
                    Store (ResourceTemplate ()
                    {
                        Interrupt (
                            ResourceConsumer, Level, ActiveHigh, Exclusive,
                            ,, _Y01)
                            { 0 } /* _INT */
                    }, Local1)

                    CreateDWordField(Local1, \_SB.PCI1.GSI2._PRS._Y01._INT, INT)
                    Store(\_SB.INT1, INT)
                    Add(INT, 2, INT)
                    Return(Local1)
                }
                Method (_CRS, 0, NotSerialized)
                {
                    Store (ResourceTemplate ()
                    {
                        Interrupt (
                            ResourceConsumer, Level, ActiveHigh, Exclusive,
                            ,, _Y01)
                            { 0 } /* _INT */
                    }, Local1)

                    CreateDWordField(Local1, \_SB.PCI1.GSI2._CRS._Y01._INT, INT)
                    Store(\_SB.INT1, INT)
                    Add(INT, 2, INT)
                    Return(Local1)
                }
                Method (_SRS, 1, NotSerialized)
                {
                }
            }

            Device (GSI3)
            {
                Name (_HID, EISAID("PNP0C0F"))
                Name (_UID, 3)
                Method (_PRS, 0, NotSerialized)
                {
                    Store (ResourceTemplate ()
                    {
                        Interrupt (
                            ResourceConsumer, Level, ActiveHigh, Exclusive,
                            ,, _Y01)
                            { 0 } /* _INT */
                    }, Local1)

                    CreateDWordField(Local1, \_SB.PCI1.GSI3._PRS._Y01._INT, INT)
                    Store(\_SB.INT1, INT)
                    Add(INT, 3, INT)
                    Return(Local1)
                }
                Method (_CRS, 0, NotSerialized)
                {
                    Store (ResourceTemplate ()
                    {
                        Interrupt (
                            ResourceConsumer, Level, ActiveHigh, Exclusive,
                            ,, _Y01)
                            { 0 } /* _INT */
                    }, Local1)

                    CreateDWordField(Local1, \_SB.PCI1.GSI3._CRS._Y01._INT, INT)
                    Store(\_SB.INT1, INT)
                    Add(INT, 3, INT)
                    Return(Local1)
                }
                Method (_SRS, 1, NotSerialized)
                {
                }
            }

            Name(_PRT, Package()
            {
                Package () {0x0001FFFF, 0, GSI1, 0},
                Package () {0x0001FFFF, 1, GSI2, 0},
                Package () {0x0001FFFF, 2, GSI3, 0},
                Package () {0x0001FFFF, 3, GSI0, 0},
                Package () {0x0002FFFF, 0, GSI2, 0},
                Package () {0x0002FFFF, 1, GSI3, 0},
                Package () {0x0002FFFF, 2, GSI0, 0},
                Package () {0x0002FFFF, 3, GSI1, 0},
                Package () {0x0003FFFF, 0, GSI3, 0},
                Package () {0x0003FFFF, 1, GSI0, 0},
                Package () {0x0003FFFF, 2, GSI1, 0},
                Package () {0x0003FFFF, 3, GSI2, 0},
                Package () {0x0004FFFF, 0, GSI0, 0},
                Package () {0x0004FFFF, 1, GSI1, 0},
                Package () {0x0004FFFF, 2, GSI2, 0},
                Package () {0x0004FFFF, 3, GSI3, 0},
                Package () {0x0005FFFF, 0, GSI1, 0},
                Package () {0x0005FFFF, 1, GSI2, 0},
                Package () {0x0005FFFF, 2, GSI3, 0},
                Package () {0x0005FFFF, 3, GSI0, 0},
                Package () {0x0006FFFF, 0, GSI2, 0},
                Package () {0x0006FFFF, 1, GSI3, 0},
                Package () {0x0006FFFF, 2, GSI0, 0},
                Package () {0x0006FFFF, 3, GSI1, 0},
                Package () {0x0007FFFF, 0, GSI3, 0},
                Package () {0x0007FFFF, 1, GSI0, 0},
                Package () {0x0007FFFF, 2, GSI1, 0},
                Package () {0x0007FFFF, 3, GSI2, 0},
                Package () {0x0008FFFF, 0, GSI0, 0},
                Package () {0x0008FFFF, 1, GSI1, 0},
                Package () {0x0008FFFF, 2, GSI2, 0},
                Package () {0x0008FFFF, 3, GSI3, 0},
                Package () {0x0009FFFF, 0, GSI1, 0},
                Package () {0x0009FFFF, 1, GSI2, 0},
                Package () {0x0009FFFF, 2, GSI3, 0},
                Package () {0x0009FFFF, 3, GSI0, 0},
                Package () {0x000aFFFF, 0, GSI2, 0},
                Package () {0x000aFFFF, 1, GSI3, 0},
                Package () {0x000aFFFF, 2, GSI0, 0},
                Package () {0x000aFFFF, 3, GSI1, 0},
                Package () {0x000bFFFF, 0, GSI3, 0},
                Package () {0x000bFFFF, 1, GSI0, 0},
                Package () {0x000bFFFF, 2, GSI1, 0},
                Package () {0x000bFFFF, 3, GSI2, 0},
                Package () {0x000cFFFF, 0, GSI0, 0},
                Package () {0x000cFFFF, 1, GSI1, 0},
                Package () {0x000cFFFF, 2, GSI2, 0},
                Package () {0x000cFFFF, 3, GSI3, 0},
                Package () {0x000dFFFF, 0, GSI1, 0},
                Package () {0x000dFFFF, 1, GSI2, 0},
                Package () {0x000dFFFF, 2, GSI3, 0},
                Package () {0x000dFFFF, 3, GSI0, 0},
                Package () {0x000eFFFF, 0, GSI2, 0},
                Package () {0x000eFFFF, 1, GSI3, 0},
                Package () {0x000eFFFF, 2, GSI0, 0},
                Package () {0x000eFFFF, 3, GSI1, 0},
                Package () {0x000fFFFF, 0, GSI3, 0},
                Package () {0x000fFFFF, 1, GSI0, 0},
                Package () {0x000fFFFF, 2, GSI1, 0},
                Package () {0x000fFFFF, 3, GSI2, 0},
                Package () {0x0010FFFF, 0, GSI0, 0},
                Package () {0x0010FFFF, 1, GSI1, 0},
                Package () {0x0010FFFF, 2, GSI2, 0},
                Package () {0x0010FFFF, 3, GSI3, 0},
                Package () {0x0011FFFF, 0, GSI1, 0},
                Package () {0x0011FFFF, 1, GSI2, 0},
                Package () {0x0011FFFF, 2, GSI3, 0},
                Package () {0x0011FFFF, 3, GSI0, 0},
                Package () {0x0012FFFF, 0, GSI2, 0},
                Package () {0x0012FFFF, 1, GSI3, 0},
                Package () {0x0012FFFF, 2, GSI0, 0},
                Package () {0x0012FFFF, 3, GSI1, 0},
                Package () {0x0013FFFF, 0, GSI3, 0},
                Package () {0x0013FFFF, 1, GSI0, 0},
                Package () {0x0013FFFF, 2, GSI1, 0},
                Package () {0x0013FFFF, 3, GSI2, 0},
                Package () {0x0014FFFF, 0, GSI0, 0},
                Package () {0x0014FFFF, 1, GSI1, 0},
                Package () {0x0014FFFF, 2, GSI2, 0},
                Package () {0x0014FFFF, 3, GSI3, 0},
                Package () {0x0015FFFF, 0, GSI1, 0},
                Package () {0x0015FFFF, 1, GSI2, 0},
                Package () {0x0015FFFF, 2, GSI3, 0},
                Package () {0x0015FFFF, 3, GSI0, 0},
                Package () {0x0016FFFF, 0, GSI2, 0},
                Package () {0x0016FFFF, 1, GSI3, 0},
                Package () {0x0016FFFF, 2, GSI0, 0},
                Package () {0x0016FFFF, 3, GSI1, 0},
                Package () {0x0017FFFF, 0, GSI3, 0},
                Package () {0x0017FFFF, 1, GSI0, 0},
                Package () {0x0017FFFF, 2, GSI1, 0},
                Package () {0x0017FFFF, 3, GSI2, 0},
                Package () {0x0018FFFF, 0, GSI0, 0},
                Package () {0x0018FFFF, 1, GSI1, 0},
                Package () {0x0018FFFF, 2, GSI2, 0},
                Package () {0x0018FFFF, 3, GSI3, 0},
                Package () {0x0019FFFF, 0, GSI1, 0},
                Package () {0x0019FFFF, 1, GSI2, 0},
                Package () {0x0019FFFF, 2, GSI3, 0},
                Package () {0x0019FFFF, 3, GSI0, 0},
                Package () {0x001aFFFF, 0, GSI2, 0},
                Package () {0x001aFFFF, 1, GSI3, 0},
                Package () {0x001aFFFF, 2, GSI0, 0},
                Package () {0x001aFFFF, 3, GSI1, 0},
                Package () {0x001bFFFF, 0, GSI3, 0},
                Package () {0x001bFFFF, 1, GSI0, 0},
                Package () {0x001bFFFF, 2, GSI1, 0},
                Package () {0x001bFFFF, 3, GSI2, 0},
                Package () {0x001cFFFF, 0, GSI0, 0},
                Package () {0x001cFFFF, 1, GSI1, 0},
                Package () {0x001cFFFF, 2, GSI2, 0},
                Package () {0x001cFFFF, 3, GSI3, 0},
                Package () {0x001dFFFF, 0, GSI1, 0},
                Package () {0x001dFFFF, 1, GSI2, 0},
                Package () {0x001dFFFF, 2, GSI3, 0},
                Package () {0x001dFFFF, 3, GSI0, 0},
                Package () {0x001eFFFF, 0, GSI2, 0},
                Package () {0x001eFFFF, 1, GSI3, 0},
                Package () {0x001eFFFF, 2, GSI0, 0},
                Package () {0x001eFFFF, 3, GSI1, 0},
                Package () {0x001fFFFF, 0, GSI3, 0},
                Package () {0x001fFFFF, 1, GSI0, 0},
                Package () {0x001fFFFF, 2, GSI1, 0},
                Package () {0x001fFFFF, 3, GSI2, 0},
            })
        }
    }
}
