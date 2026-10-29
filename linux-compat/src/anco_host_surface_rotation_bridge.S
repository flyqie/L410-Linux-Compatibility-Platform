/* Original6.0 GraphicBroker -> originalSP68 ProducerDelegator ABI.
 * BOTH verified original static methods take int32 in w0 and release-store
 * it into the producer's global rotation. No this/object/extra parameter.
 * Only class-name relocation: execute the original new method; do not emulate
 * its state, return success, approximate rotation, or modify either library.
 * Old body0x726b4/16; SP68 CFI entry0x1cd74 -> real body0x174a0/16.
 */
.text
.p2align 2
.global _ZN4OHOS24ProducerSurfaceDelegator18SetDisplayRotationEi
.type _ZN4OHOS24ProducerSurfaceDelegator18SetDisplayRotationEi, %function
_ZN4OHOS24ProducerSurfaceDelegator18SetDisplayRotationEi:
    b _ZN4OHOS17ProducerDelegator18SetDisplayRotationEi
.size _ZN4OHOS24ProducerSurfaceDelegator18SetDisplayRotationEi, .-_ZN4OHOS24ProducerSurfaceDelegator18SetDisplayRotationEi
.section .note.GNU-stack,"",%progbits
