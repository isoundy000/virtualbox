/* $Id$ */
/** @file
 * VirtualBox Guest Additions Driver for Solaris.
 */

/*
 * Copyright (C) 2007 innotek GmbH
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * in version 2 as it comes in the "COPYING" file of the VirtualBox OSE
 * distribution. VirtualBox OSE is distributed in the hope that it will
 * be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include <sys/conf.h>
#include <sys/cmn_err.h>
#include <sys/modctl.h>
#include <sys/mutex.h>
#include <sys/pci.h>
#include <sys/stat.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#undef u /* /usr/include/sys/user.h:249:1 is where this is defined to (curproc->p_user). very cool. */

#include "VBoxGuestInternal.h"
#include <VBox/log.h>
#include <VBox/VBoxGuest.h>
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/initterm.h>
#include <iprt/process.h>
#include <iprt/mem.h>


/*******************************************************************************
*   Defined Constants And Macros                                               *
*******************************************************************************/
/** The module name. */
#define DEVICE_NAME              "vboxadd"
/** The module description as seen in 'modinfo'. */
#define DEVICE_DESC              "VirtualBox Guest Additions Driver"
/** @name R0 Log helpers.
 * @{ */
#define VBA_LOGCONT(...)         cmn_err(CE_CONT, "vboxadd: " __VA_ARGS__);
#define VBA_LOGNOTE(...)         cmn_err(CE_NOTE, "vboxadd: " __VA_ARGS__);
/** @} */


/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
static int VBoxAddSolarisOpen(dev_t *pDev, int fFlag, int fType, cred_t *pCred);
static int VBoxAddSolarisClose(dev_t Dev, int fFlag, int fType, cred_t *pCred);
static int VBoxAddSolarisRead(dev_t Dev, struct uio *pUio, cred_t *pCred);
static int VBoxAddSolarisWrite(dev_t Dev, struct uio *pUio, cred_t *pCred);
static int VBoxAddSolarisIOCtl(dev_t Dev, int Cmd, intptr_t pArg, int mode, cred_t *pCred, int *pVal);

static int VBoxAddSolarisGetInfo(dev_info_t *pDip, ddi_info_cmd_t enmCmd, void *pArg, void **ppResult);
static int VBoxAddSolarisAttach(dev_info_t *pDip, ddi_attach_cmd_t enmCmd);
static int VBoxAddSolarisDetach(dev_info_t *pDip, ddi_detach_cmd_t enmCmd);

static int VBoxGuestSolarisAddIRQ(dev_info_t *pDip, void *pvState);
static void VBoxGuestSolarisRemoveIRQ(dev_info_t *pDip, void *pvState);
static uint_t VBoxGuestSolarisISR(caddr_t Arg);

DECLVBGL(int) VBoxGuestSolarisServiceCall(void *pvSession, unsigned iCmd, void *pvData, size_t cbData, size_t *pcbDataReturned);
DECLVBGL(void *) VBoxGuestSolarisServiceOpen(uint32_t *pu32Version);
DECLVBGL(int) VBoxGuestSolarisServiceClose(void *pvSession);


/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
/**
 * cb_ops: for drivers that support char/block entry points
 */
static struct cb_ops g_VBoxAddSolarisCbOps =
{
    VBoxAddSolarisOpen,
    VBoxAddSolarisClose,
    nodev,                  /* b strategy */
    nodev,                  /* b dump */
    nodev,                  /* b print */
    VBoxAddSolarisRead,
    VBoxAddSolarisWrite,
    VBoxAddSolarisIOCtl,
    nodev,                  /* c devmap */
    nodev,                  /* c mmap */
    nodev,                  /* c segmap */
    nochpoll,               /* c poll */
    ddi_prop_op,            /* property ops */
    NULL,                   /* streamtab  */
    D_NEW | D_MP,           /* compat. flag */
    CB_REV                  /* revision */
};

/**
 * dev_ops: for driver device operations
 */
static struct dev_ops g_VBoxAddSolarisDevOps =
{
    DEVO_REV,               /* driver build revision */
    0,                      /* ref count */
    VBoxAddSolarisGetInfo,
    nulldev,                /* identify */
    nulldev,                /* probe */
    VBoxAddSolarisAttach,
    VBoxAddSolarisDetach,
    nodev,                  /* reset */
    &g_VBoxAddSolarisCbOps,
    (struct bus_ops *)0,
    nodev                   /* power */
};

/**
 * modldrv: export driver specifics to the kernel
 */
static struct modldrv g_VBoxAddSolarisModule =
{
    &mod_driverops,         /* extern from kernel */
    DEVICE_DESC,
    &g_VBoxAddSolarisDevOps
};

/**
 * modlinkage: export install/remove/info to the kernel
 */
static struct modlinkage g_VBoxAddSolarisModLinkage =
{
    MODREV_1,               /* loadable module system revision */
    &g_VBoxAddSolarisModule,
    NULL                    /* terminate array of linkage structures */
};

/**
 * State info for each open file handle.
 */
typedef struct
{
    /** PCI handle of VMMDev. */
    ddi_acc_handle_t        PciHandle;
    /** IO port handle. */
    ddi_acc_handle_t        PciIOHandle;
    /** MMIO handle. */
    ddi_acc_handle_t        PciMMIOHandle;
    /** Interrupt block cookie. */
    ddi_iblock_cookie_t     BlockCookie;
    /** Driver Mutex. */
    kmutex_t                Mtx;
    /** IO Port. */
    uint16_t                uIOPortBase;
    /** Address of the MMIO region.*/
    caddr_t                 pMMIOBase;
    /** Size of the MMIO region. */
    off_t                   cbMMIO;
    /** VMMDev Version. */
    uint32_t                u32Version;
} VBoxAddDevState;

/** Device handle (we support only one instance). */
static dev_info_t *g_pDip;

/** Opaque pointer to state */
static void *g_pVBoxAddSolarisState;

/** Device extention & session data association structure. */
static VBOXGUESTDEVEXT      g_DevExt;
/** Spinlock protecting g_apSessionHashTab. */
static RTSPINLOCK           g_Spinlock = NIL_RTSPINLOCK;
/** Hash table */
static PVBOXGUESTSESSION    g_apSessionHashTab[19];
/** Calculates the index into g_apSessionHashTab.*/
#define SESSION_HASH(sfn) ((sfn) % RT_ELEMENTS(g_apSessionHashTab))

/** GCC C++ hack. */
unsigned __gxx_personality_v0 = 0xdecea5ed;

/**
 * Kernel entry points
 */
int _init(void)
{
    VBA_LOGCONT("_init\n");

    int rc = ddi_soft_state_init(&g_pVBoxAddSolarisState, sizeof(VBoxAddDevState), 1);
    if (!rc)
    {
        rc = mod_install(&g_VBoxAddSolarisModLinkage);
        if (rc)
            ddi_soft_state_fini(&g_pVBoxAddSolarisState);
    }
    return rc;
}


int _fini(void)
{
    VBA_LOGCONT("_fini\n");

    int rc = mod_remove(&g_VBoxAddSolarisModLinkage);
    if (!rc)
        ddi_soft_state_fini(&g_pVBoxAddSolarisState);
    return rc;
}


int _info(struct modinfo *pModInfo)
{
    VBA_LOGCONT("_info\n");
    return mod_info(&g_VBoxAddSolarisModLinkage, pModInfo);
}


/**
 * Attach entry point, to attach a device to the system or resume it.
 *
 * @param   pDip            The module structure instance.
 * @param   enmCmd          Attach type (ddi_attach_cmd_t)
 *
 * @return  corresponding solaris error code.
 */
static int VBoxAddSolarisAttach(dev_info_t *pDip, ddi_attach_cmd_t enmCmd)
{
    VBA_LOGCONT("VBoxAddSolarisAttach\n");
    switch (enmCmd)
    {
        case DDI_ATTACH:
        {
            int rc;
            int instance;
            VBoxAddDevState *pState;

            instance = ddi_get_instance(pDip);
            rc = ddi_soft_state_zalloc(g_pVBoxAddSolarisState, instance);
            if (rc != DDI_SUCCESS)
            {
                VBA_LOGNOTE("ddi_soft_state_zalloc failed.\n");
                return DDI_FAILURE;
            }

            pState = ddi_get_soft_state(g_pVBoxAddSolarisState, instance);
            if (!pState)
            {
                ddi_soft_state_free(g_pVBoxAddSolarisState, instance);
                VBA_LOGNOTE("ddi_get_soft_state for instance %d failed\n", instance);
                return DDI_FAILURE;
            }

            /*
             * Initialize IPRT R0 driver, which internally calls OS-specific r0 init.
             */
            rc = RTR0Init(0);
            if (RT_FAILURE(rc))
            {
                VBA_LOGNOTE("RTR0Init failed.\n");
                return DDI_FAILURE;
            }

            /*
             * Initialize the session hash table.
             */
            rc = RTSpinlockCreate(&g_Spinlock);
            if (RT_SUCCESS(rc))
            {
                /*
                 * Enable resources for PCI access.
                 */
                rc = pci_config_setup(pDip, &pState->PciHandle);
                if (rc == DDI_SUCCESS)
                {
                    /*
                     * Check vendor and device ID.
                     */
                    uint16_t uVendorID = pci_config_get16(pState->PciHandle, PCI_CONF_VENID);
                    uint16_t uDeviceID = pci_config_get16(pState->PciHandle, PCI_CONF_DEVID);
                    if (   uVendorID == VMMDEV_VENDORID
                        && uDeviceID == VMMDEV_DEVICEID)
                    {
                        /*
                         * Verify PCI class of the device (a bit paranoid).
                         */
                        uint8_t uClass = pci_config_get8(pState->PciHandle, PCI_CONF_BASCLASS);
                        uint8_t uSubClass = pci_config_get8(pState->PciHandle, PCI_CONF_SUBCLASS);
                        if (   uClass == PCI_CLASS_PERIPH
                            && uSubClass == PCI_PERIPH_OTHER)
                        {
                            /*
                             * Map the register address space.
                             */
                            caddr_t baseAddr;
                            ddi_device_acc_attr_t deviceAttr;
                            deviceAttr.devacc_attr_version = DDI_DEVICE_ATTR_V0;
                            deviceAttr.devacc_attr_endian_flags = DDI_NEVERSWAP_ACC;
                            deviceAttr.devacc_attr_dataorder = DDI_STRICTORDER_ACC;
                            deviceAttr.devacc_attr_access = DDI_DEFAULT_ACC;
                            rc = ddi_regs_map_setup(pDip, 1, &baseAddr, 0, 0, &deviceAttr, &pState->PciIOHandle);
                            if (rc == DDI_SUCCESS)
                            {
                                /*
                                 * Read size of the MMIO region.
                                 */
                                pState->uIOPortBase = (uintptr_t)baseAddr;
                                rc = ddi_dev_regsize(pDip, 2, &pState->cbMMIO);
                                if (rc == DDI_SUCCESS)
                                {
                                    rc = ddi_regs_map_setup(pDip, 2, &pState->pMMIOBase, 0, pState->cbMMIO, &deviceAttr,
                                                &pState->PciMMIOHandle);
                                    if (rc == DDI_SUCCESS)
                                    {
                                        /*
                                         * Add IRQ of VMMDev.
                                         */
                                        rc = VBoxGuestSolarisAddIRQ(pDip, pState);
                                        if (rc == DDI_SUCCESS)
                                        {
                                            /*
                                             * Call the common device extension initializer.
                                             */
                                            rc = VBoxGuestInitDevExt(&g_DevExt, pState->uIOPortBase, pState->pMMIOBase,
                                                        pState->cbMMIO, OSTypeSolaris);
                                            if (RT_SUCCESS(rc))
                                            {
                                                rc = ddi_create_minor_node(pDip, DEVICE_NAME, S_IFCHR, instance, DDI_PSEUDO, 0);
                                                if (rc == DDI_SUCCESS)
                                                {
                                                    g_pDip = pDip;
                                                    ddi_set_driver_private(pDip, pState);
                                                    pci_config_teardown(&pState->PciHandle);
                                                    ddi_report_dev(pDip);
                                                    return DDI_SUCCESS;
                                                }

                                                VBA_LOGNOTE("ddi_create_minor_node failed.\n");
                                            }
                                            else
                                                VBA_LOGNOTE("VBoxGuestInitDevExt failed.\n");
                                            VBoxGuestSolarisRemoveIRQ(pDip, pState);
                                        }
                                        else
                                            VBA_LOGNOTE("VBoxGuestSolarisAddIRQ failed.\n");
                                        ddi_regs_map_free(&pState->PciMMIOHandle);
                                    }
                                    else
                                        VBA_LOGNOTE("ddi_regs_map_setup for MMIO region failed.\n");
                                }
                                else
                                    VBA_LOGNOTE("ddi_dev_regsize for MMIO region failed.\n");
                                    ddi_regs_map_free(&pState->PciIOHandle);
                            }
                            else
                                VBA_LOGNOTE("ddi_regs_map_setup for IOport failed.\n");
                        }
                        else
                            VBA_LOGNOTE("PCI class/sub-class does not match.\n");
                    }
                    else
                        VBA_LOGNOTE("PCI vendorID, deviceID does not match.\n");
                    pci_config_teardown(&pState->PciHandle);
                }
                else
                    VBA_LOGNOTE("pci_config_setup failed rc=%d.\n", rc);
                RTSpinlockDestroy(g_Spinlock);
                g_Spinlock = NIL_RTSPINLOCK;
            }
            else
                VBA_LOGNOTE("RTSpinlockCreate failed.\n");

            RTR0Term();
            return DDI_FAILURE;
        }

        case DDI_RESUME:
        {
            /** @todo implement resume for guest driver. */
            return DDI_SUCCESS;
        }

        default:
            return DDI_FAILURE;
    }
}


/**
 * Detach entry point, to detach a device to the system or suspend it.
 *
 * @param   pDip            The module structure instance.
 * @param   enmCmd          Attach type (ddi_attach_cmd_t)
 *
 * @return  corresponding solaris error code.
 */
static int VBoxAddSolarisDetach(dev_info_t *pDip, ddi_detach_cmd_t enmCmd)
{
    VBA_LOGCONT("VBoxAddSolarisDetach\n");
    switch (enmCmd)
    {
        case DDI_DETACH:
        {
            int rc;
            int instance = ddi_get_instance(pDip);
            VBoxAddDevState *pState = ddi_get_soft_state(g_pVBoxAddSolarisState, instance);
            if (pState)
            {
                VBoxGuestSolarisRemoveIRQ(pDip, pState);
                ddi_regs_map_free(&pState->PciIOHandle);
                ddi_regs_map_free(&pState->PciMMIOHandle);
                ddi_remove_minor_node(pDip, NULL);
                ddi_soft_state_free(g_pVBoxAddSolarisState, instance);

                rc = RTSpinlockDestroy(g_Spinlock);
                AssertRC(rc);
                g_Spinlock = NIL_RTSPINLOCK;

                RTR0Term();
                return DDI_SUCCESS;
            }
            VBA_LOGNOTE("ddi_get_soft_state failed. Cannot detach instance %d\n", instance);
            return DDI_FAILURE;
        }

        case DDI_SUSPEND:
        {
            /** @todo implement suspend for guest driver. */
            return DDI_SUCCESS;
        }

        default:
            return DDI_FAILURE;
    }
}


/**
 * Info entry point, called by solaris kernel for obtaining driver info.
 *
 * @param   pDip            The module structure instance (do not use).
 * @param   enmCmd          Information request type.
 * @param   pArg            Type specific argument.
 * @param   ppResult        Where to store the requested info.
 *
 * @return  corresponding solaris error code.
 */
static int VBoxAddSolarisGetInfo(dev_info_t *pDip, ddi_info_cmd_t enmCmd, void *pArg, void **ppResult)
{
    VBA_LOGCONT("VBoxAddSolarisGetInfo\n");

    int rc = DDI_SUCCESS;
    switch (enmCmd)
    {
        case DDI_INFO_DEVT2DEVINFO:
            *ppResult = (void *)g_pDip;
            break;

        case DDI_INFO_DEVT2INSTANCE:
            *ppResult = (void *)ddi_get_instance(g_pDip);
            break;

        default:
            rc = DDI_FAILURE;
            break;
    }
    return rc;
}


/**
 * User context entry points
 */
static int VBoxAddSolarisOpen(dev_t *pDev, int fFlag, int fType, cred_t *pCred)
{
    int                 rc;
    PVBOXGUESTSESSION   pSession;

    VBA_LOGCONT("VBoxAddSolarisOpen\n");

    /*
     * Create a new session.
     */
    rc = VBoxGuestCreateUserSession(&g_DevExt, &pSession);
    if (RT_SUCCESS(rc))
    {
        /*
         * Insert it into the hash table.
         */
        unsigned iHash = SESSION_HASH(pSession->Process);
        RTSPINLOCKTMP Tmp = RTSPINLOCKTMP_INITIALIZER;
        RTSpinlockAcquireNoInts(g_Spinlock, &Tmp);
        pSession->pNextHash = g_apSessionHashTab[iHash];
        g_apSessionHashTab[iHash] = pSession;
        RTSpinlockReleaseNoInts(g_Spinlock, &Tmp);

        Log(("VBoxAddSolarisOpen: g_DevExt=%p pSession=%p rc=%d pid=%d\n", &g_DevExt, pSession, rc, (int)RTProcSelf()));
        return 0;
    }
    LogRel(("VBoxAddSolarisOpen: VBoxGuestCreateUserSession failed. rc=%d\n", rc));
    return rc;
}


static int VBoxAddSolarisClose(dev_t Dev, int flag, int fType, cred_t *pCred)
{
    VBA_LOGCONT("VBoxAddSolarisClose pid=%d=%d\n", (int)RTProcSelf());

    /*
     * Remove from the hash table.
     */
    PVBOXGUESTSESSION   pSession;
    const RTPROCESS     Process = RTProcSelf();
    const unsigned      iHash = SESSION_HASH(Process);
    RTSPINLOCKTMP       Tmp = RTSPINLOCKTMP_INITIALIZER;
    RTSpinlockAcquireNoInts(g_Spinlock, &Tmp);

    pSession = g_apSessionHashTab[iHash];
    if (pSession)
    {
        if (pSession->Process == Process)
        {
            g_apSessionHashTab[iHash] = pSession->pNextHash;
            pSession->pNextHash = NULL;
        }
        else
        {
            PVBOXGUESTSESSION pPrev = pSession;
            pSession = pSession->pNextHash;
            while (pSession)
            {
                if (pSession->Process == Process)
                {
                    pPrev->pNextHash = pSession->pNextHash;
                    pSession->pNextHash = NULL;
                    break;
                }

                /* next */
                pPrev = pSession;
                pSession = pSession->pNextHash;
            }
        }
    }
    RTSpinlockReleaseNoInts(g_Spinlock, &Tmp);
    if (!pSession)
    {
        Log(("VBoxGuestIoctl: WHUT?!? pSession == NULL! This must be a mistake... pid=%d", (int)Process));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Close the session.
     */
    VBoxGuestCloseSession(&g_DevExt, pSession);
    return 0;
}


static int VBoxAddSolarisRead(dev_t Dev, struct uio *pUio, cred_t *pCred)
{
    VBA_LOGCONT("VBoxAddSolarisRead\n");
    return DDI_SUCCESS;
}


static int VBoxAddSolarisWrite(dev_t Dev, struct uio *pUio, cred_t *pCred)
{
    VBA_LOGCONT("VBoxAddSolarisWrite\n");
    return DDI_SUCCESS;
}

/** @def IOCPARM_LEN
 * Gets the length from the ioctl number.
 * This is normally defined by sys/ioccom.h on BSD systems...
 */
#ifndef IOCPARM_LEN
# define IOCPARM_LEN(x)     ( ((x) >> 16) & IOCPARM_MASK )
#endif

/**
 * Driver ioctl, an alternate entry point for this character driver.
 *
 * @param   Dev             Device number
 * @param   Cmd             Operation identifier
 * @param   pArg            Arguments from user to driver
 * @param   Mode            Information bitfield (read/write, address space etc.)
 * @param   pCred           User credentials
 * @param   pVal            Return value for calling process.
 *
 * @return  corresponding solaris error code.
 */
static int VBoxAddSolarisIOCtl(dev_t Dev, int Cmd, intptr_t pArg, int Mode, cred_t *pCred, int *pVal)
{
    VBA_LOGCONT("VBoxAddSolarisIOCtl\n");

    /** @todo use the faster way to find pSession (using the soft state) */
    RTSPINLOCKTMP       Tmp = RTSPINLOCKTMP_INITIALIZER;
    const RTPROCESS     Process = RTProcSelf();
    const unsigned      iHash = SESSION_HASH(Process);
    PVBOXGUESTSESSION   pSession;

    /*
     * Find the session.
     */
    RTSpinlockAcquireNoInts(g_Spinlock, &Tmp);
    pSession = g_apSessionHashTab[iHash];
    if (pSession && pSession->Process != Process)
    {
        do pSession = pSession->pNextHash;
        while (pSession && pSession->Process != Process);
    }
    RTSpinlockReleaseNoInts(g_Spinlock, &Tmp);
    if (!pSession)
    {
        VBA_LOGNOTE("VBoxAddSolarisIOCtl: WHAT?!? pSession == NULL! This must be a mistake... pid=%d iCmd=%#x\n",
                    (int)Process, Cmd);
        return EINVAL;
    }

    /** @todo I'll remove this size check after testing. */
    uint32_t cbBuf = 0;
    if (    Cmd >= VBOXGUEST_IOCTL_VMMREQUEST(0)
        &&  Cmd <= VBOXGUEST_IOCTL_VMMREQUEST(0xfff))
        cbBuf = sizeof(VMMDevRequestHeader);
#ifdef VBOX_HGCM
    else if (   Cmd >= VBOXGUEST_IOCTL_HGCM_CALL(0)
             && Cmd <= VBOXGUEST_IOCTL_HGCM_CALL(0xfff))
        cbBuf = sizeof(VBoxGuestHGCMCallInfo);
#endif /* VBOX_HGCM */
    else
    {
        switch (Cmd)
        {
            case VBOXGUEST_IOCTL_GETVMMDEVPORT:
                cbBuf = sizeof(VBoxGuestPortInfo);
                break;

            case VBOXGUEST_IOCTL_WAITEVENT:
                cbBuf = sizeof(VBoxGuestWaitEventInfo);
                break;

            case VBOXGUEST_IOCTL_CTL_FILTER_MASK:
                cbBuf = sizeof(VBoxGuestFilterMaskInfo);
                break;

#ifdef VBOX_HGCM
            case VBOXGUEST_IOCTL_HGCM_CONNECT:
                cbBuf = sizeof(VBoxGuestHGCMConnectInfo);
                break;

            case VBOXGUEST_IOCTL_HGCM_DISCONNECT:
                cbBuf = sizeof(VBoxGuestHGCMDisconnectInfo);
                break;

            case VBOXGUEST_IOCTL_CLIPBOARD_CONNECT:
                cbBuf = sizeof(uint32_t);
                break;
#endif /* VBOX_HGCM */

            default:
            {
                VBA_LOGNOTE("VBoxAddSolarisIOCtl: Unkown request %d\n", Cmd);
                return VERR_NOT_SUPPORTED;
            }
        }
    }

    if (RT_UNLIKELY(cbBuf != IOCPARM_LEN(Cmd)))
    {
        VBA_LOGNOTE("VBoxAddSolarisIOCtl: buffer size mismatch. size=%d expected=%d.\n", IOCPARM_LEN(Cmd), cbBuf);
        return EINVAL;
    }

    void *pvBuf = RTMemTmpAlloc(cbBuf);
    if (RT_UNLIKELY(!pvBuf))
    {
        VBA_LOGNOTE("VBoxAddSolarisIOCtl: RTMemTmpAlloc failed to alloc %d bytes.\n", cbBuf);
        return ENOMEM;
    }

    int rc = ddi_copyin((void *)pArg, pvBuf, cbBuf, Mode);
    if (RT_UNLIKELY(rc))
    {
        RTMemTmpFree(pvBuf);
        VBA_LOGNOTE("VBoxAddSolarisIOCtl: ddi_copyin failed; pvBuf=%p pArg=%p Cmd=%d. rc=%d\n", pvBuf, pArg, Cmd, rc);
        return EFAULT;
    }

    size_t cbDataReturned;
    rc = VBoxGuestCommonIOCtl(Cmd, &g_DevExt, pSession, pvBuf, cbBuf, &cbDataReturned);
    if (RT_LIKELY(!rc))
    {
        if (RT_UNLIKELY(cbDataReturned > cbBuf))
        {
            VBA_LOGNOTE("VBoxAddSolarisIOCtl: too much output data %d expected %d\n", cbDataReturned, cbBuf);
            cbDataReturned = cbBuf;
        }
        rc = ddi_copyout(pvBuf, (void *)pArg, cbDataReturned, Mode);
        if (RT_UNLIKELY(rc))
        {
            VBA_LOGNOTE("VBoxAddSolarisIOCtl: ddi_copyout failed; pvBuf=%p pArg=%p Cmd=%d. rc=%d\n", pvBuf, pArg, Cmd, rc);
            rc = EFAULT;
        }
    }
    else
    {
        VBA_LOGNOTE("VBoxAddSolarisIOCtl: VBoxGuestCommonIOCtl failed. rc=%d\n", rc);
        rc = EFAULT;
    }
    *pVal = rc;
    RTMemTmpFree(pvBuf);
    return rc;
}


/**
 * Sets IRQ for VMMDev.
 *
 * @returns Solaris error code.
 * @param   pDip     Pointer to the device info structure.
 * @param   pvState  Pointer to the state info structure.
 */
static int VBoxGuestSolarisAddIRQ(dev_info_t *pDip, void *pvState)
{
    int rc;
    VBoxAddDevState *pState = (VBoxAddDevState *)pvState;
    VBA_LOGCONT("VBoxGuestSolarisAddIRQ\n");

    /*
     * These calls are supposedly deprecated. But Sun seems to use them all over
     * the place. Anyway, once this works we will switch to the highly elaborate
     * and non-obsolete way of setting up IRQs.
     */
    rc = ddi_get_iblock_cookie(pDip, 0, &pState->BlockCookie);
    if (rc == DDI_SUCCESS)
    {
        mutex_init(&pState->Mtx, "VBoxGuest Driver Mutex", MUTEX_DRIVER, (void *)pState->BlockCookie);
        rc = ddi_add_intr(pDip, 0, &pState->BlockCookie, NULL, VBoxGuestSolarisISR, (caddr_t)pState);
        if (rc != DDI_SUCCESS)
            VBA_LOGNOTE("ddi_add_intr failed. Cannot set IRQ for VMMDev.\n");
    }
    else
        VBA_LOGNOTE("ddi_get_iblock_cookie failed. Cannot set IRQ for VMMDev.\n");
    return rc;
}


/**
 * Removes IRQ for VMMDev.
 *
 * @param   pDip     Pointer to the device info structure.
 * @param   pvState  Opaque pointer to the state info structure.
 */
static void VBoxGuestSolarisRemoveIRQ(dev_info_t *pDip, void *pvState)
{
    VBA_LOGCONT("VBoxGuestSolarisRemoveIRQ\n");

    VBoxAddDevState *pState = (VBoxAddDevState *)pvState;
    ddi_remove_intr(pDip, 0, pState->BlockCookie);
    mutex_destroy(&pState->Mtx);
}


/**
 * Interrupt Service Routine for VMMDev.
 *
 * @returns DDI_INTR_CLAIMED if it's our interrupt, DDI_INTR_UNCLAIMED if it isn't.
 */
static uint_t VBoxGuestSolarisISR(caddr_t Arg)
{
    VBA_LOGCONT("VBoxGuestSolarisISR\n");

    VBoxAddDevState *pState = (VBoxAddDevState *)Arg;
    mutex_enter(&pState->Mtx);
    bool fOurIRQ = VBoxGuestCommonISR(&g_DevExt);
    mutex_exit(&pState->Mtx);

    return fOurIRQ ? DDI_INTR_CLAIMED : DDI_INTR_CLAIMED;
}


/**
 * VBoxGuest Common ioctl wrapper from VBoxGuestLib.
 *
 * @returns VBox error code.
 * @param   pvSession           Opaque pointer to the session.
 * @param   iCmd                Requested function.
 * @param   pvData              IO data buffer.
 * @param   cbData              Size of the data buffer.
 * @param   pcbDataReturned     Where to store the amount of returned data.
 */
DECLVBGL(int) VBoxGuestSolarisServiceCall(void *pvSession, unsigned iCmd, void *pvData, size_t cbData, size_t *pcbDataReturned)
{
    VBA_LOGCONT("VBoxGuestSolarisServiceCall\n");

    PVBOXGUESTSESSION pSession = (PVBOXGUESTSESSION)pvSession;
    AssertPtrReturn(pSession, VERR_INVALID_POINTER);
    AssertMsgReturn(pSession->pDevExt == &g_DevExt,
                    ("SC: %p != %p\n", pSession->pDevExt, &g_DevExt), VERR_INVALID_HANDLE);

    return VBoxGuestCommonIOCtl(iCmd, &g_DevExt, pSession, pvData, cbData, pcbDataReturned);
}


/**
 * Solaris Guest service open.
 *
 * @returns Opaque pointer to session object.
 * @param   pu32Version         Where to store VMMDev version.
 */
DECLVBGL(void *) VBoxGuestSolarisServiceOpen(uint32_t *pu32Version)
{
    VBA_LOGCONT("VBoxGuestSolarisServiceOpen\n");

    AssertPtrReturn(pu32Version, NULL);
    PVBOXGUESTSESSION pSession;
    int rc = VBoxGuestCreateKernelSession(&g_DevExt, &pSession);
    if (RT_SUCCESS(rc))
    {
        *pu32Version = VMMDEV_VERSION;
        return pSession;
    }
    VBA_LOGNOTE("VBoxGuestCreateKernelSession failed. rc=%d\n", rc);
    return NULL;
}


/**
 * Solaris Guest service close.
 *
 * @returns VBox error code.
 * @param   pvState             Opaque pointer to the session object.
 */
DECLVBGL(int) VBoxGuestSolarisServiceClose(void *pvSession)
{
    VBA_LOGCONT("VBoxGuestSolarisServiceClose\n");

    PVBOXGUESTSESSION pSession = (PVBOXGUESTSESSION)pvSession;
    AssertPtrReturn(pSession, VERR_INVALID_POINTER);
    if (pSession)
    {
        VBoxGuestCloseSession(&g_DevExt, pSession);
        return VINF_SUCCESS;
    }
    VBA_LOGNOTE("Invalid pSession.\n");
    return VERR_INVALID_HANDLE;
}

