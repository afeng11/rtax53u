// SPDX-License-Identifier: GPL-2.0
/* FILE NAME:  en8801s.c
 * PURPOSE:
 *      EN8801S phy driver for Linux
 * NOTES:
 *
 */

/* INCLUDE FILE DECLARATIONS
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/delay.h>

#include <linux/uaccess.h>
#include <linux/version.h>

#include "en8801s.h"

MODULE_DESCRIPTION("Airoha EN8801S PHY drivers");
MODULE_AUTHOR("Airoha");
MODULE_LICENSE("GPL");

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0))
#define phydev_mdio_bus(dev) ((dev)->bus)
#else
#define phydev_mdio_bus(dev) ((dev)->mdio.bus)
#endif

enum {
    PHY_STATE_DONE = 0,
    PHY_STATE_INIT = 1,
    PHY_STATE_PROCESS = 2,
};

/************************************************************************
*                  F U N C T I O N S
************************************************************************/
static unsigned int airoha_cl45_write(struct mii_bus *bus, u32 port, u32 devad, u32 reg, u16 val)
{
    mdiobus_write(bus, port, MII_MMD_ACC_CTL_REG, devad);
    mdiobus_write(bus, port, MII_MMD_ADDR_DATA_REG, reg);
    mdiobus_write(bus, port, MII_MMD_ACC_CTL_REG, MMD_OP_MODE_DATA | devad);
    mdiobus_write(bus, port, MII_MMD_ADDR_DATA_REG, val);
    return 0;
}

static unsigned int airoha_cl45_read(struct mii_bus *bus, u32 port, u32 devad, u32 reg, u32 *read_data)
{
    mdiobus_write(bus, port, MII_MMD_ACC_CTL_REG, devad);
    mdiobus_write(bus, port, MII_MMD_ADDR_DATA_REG, reg);
    mdiobus_write(bus, port, MII_MMD_ACC_CTL_REG, MMD_OP_MODE_DATA | devad);
    *read_data = mdiobus_read(bus, port, MII_MMD_ADDR_DATA_REG);
    return 0;
}

static unsigned int airoha_cl22_read(struct mii_bus *ebus, unsigned int phy_addr, unsigned int phy_register, unsigned int *read_data)
{
    *read_data = mdiobus_read(ebus, phy_addr, phy_register);
    return 0;
}

static unsigned int airoha_cl22_write(struct mii_bus *ebus, unsigned int phy_addr, unsigned int phy_register, unsigned int write_data)
{
    mdiobus_write(ebus, phy_addr, phy_register, write_data);
    return 0;
}

static void airoha_pbus_write(struct mii_bus *ebus, unsigned long pbus_id, unsigned long pbus_address, unsigned long pbus_data)
{
    airoha_cl22_write(ebus, pbus_id, 0x1F, (unsigned int)(pbus_address >> 6));
    airoha_cl22_write(ebus, pbus_id, (unsigned int)((pbus_address >> 2) & 0xf), (unsigned int)(pbus_data & 0xFFFF));
    airoha_cl22_write(ebus, pbus_id, 0x10, (unsigned int)(pbus_data >> 16));
    return;
}

static unsigned long airoha_pbus_read(struct mii_bus *ebus, unsigned long pbus_id, unsigned long pbus_address)
{
    unsigned long pbus_data;
    unsigned int pbus_data_low, pbus_data_high;

    airoha_cl22_write(ebus, pbus_id, 0x1F, (unsigned int)(pbus_address >> 6));
    airoha_cl22_read(ebus, pbus_id, (unsigned int)((pbus_address >> 2) & 0xf), &pbus_data_low);
    airoha_cl22_read(ebus, pbus_id, 0x10, &pbus_data_high);
    pbus_data = (pbus_data_high << 16) + pbus_data_low;
    return pbus_data;
}

/* Airoha Token Ring Write function */
static void airoha_tr_reg_write(struct mii_bus *ebus, unsigned long tr_address, unsigned long tr_data)
{
    airoha_cl22_write(ebus, EN8801S_MDIO_PHY_ID, 0x1F, 0x52b5);       /* page select */
    airoha_cl22_write(ebus, EN8801S_MDIO_PHY_ID, 0x11, (unsigned int)(tr_data & 0xffff));
    airoha_cl22_write(ebus, EN8801S_MDIO_PHY_ID, 0x12, (unsigned int)(tr_data >> 16));
    airoha_cl22_write(ebus, EN8801S_MDIO_PHY_ID, 0x10, (unsigned int)(tr_address | TrReg_WR));
    airoha_cl22_write(ebus, EN8801S_MDIO_PHY_ID, 0x1F, 0x0);          /* page resetore */
    return;
}

/* Airoha Token Ring Read function */
static unsigned long airoha_tr_reg_read(struct mii_bus *ebus, unsigned long tr_address)
{
    unsigned long tr_data;
    unsigned int tr_data_low, tr_data_high;

    airoha_cl22_write(ebus, EN8801S_MDIO_PHY_ID, 0x1F, 0x52b5);       /* page select */
    airoha_cl22_write(ebus, EN8801S_MDIO_PHY_ID, 0x10, (unsigned int)(tr_address | TrReg_RD));
    airoha_cl22_read(ebus, EN8801S_MDIO_PHY_ID, 0x11, &tr_data_low);
    airoha_cl22_read(ebus, EN8801S_MDIO_PHY_ID, 0x12, &tr_data_high);
    airoha_cl22_write(ebus, EN8801S_MDIO_PHY_ID, 0x1F, 0x0);          /* page resetore */
    tr_data = (tr_data_high << 16) + tr_data_low;
    return tr_data;
}

static void en8801s_led_init(struct phy_device *phydev)
{
    struct mii_bus *mbus = phydev_mdio_bus(phydev);
    u32 reg_value;

    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x186c, 0x3);
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0X1870, 0x100);
    reg_value = (airoha_pbus_read(mbus, EN8801S_PBUS_PHY_ID, 0x1880) & ~(0x3));
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x1880, reg_value);
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1f, 0x21, 0x8008);
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1f, 0x22, 0x600);
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1f, 0x23, 0xc00);
    /* LED0: 10M/100M */
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1f, 0x24, 0x8006);
    /* LED0: blink 10M/100M Tx/Rx */
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1f, 0x25, 0x3c);
    /* LED1: 1000M */
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1f, 0x26, 0x8001);
    /* LED1: blink 1000M Tx/Rx */
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1f, 0x27, 0x3);
}

static int en8801s_phy_process(struct phy_device *phydev)
{
    struct mii_bus *mbus = phydev_mdio_bus(phydev);
    u32 reg_value = 0;

    reg_value = airoha_pbus_read(mbus, EN8801S_PBUS_PHY_ID, 0x19e0);
    reg_value |= (1 << 0);
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x19e0, reg_value);
    reg_value = airoha_pbus_read(mbus, EN8801S_PBUS_PHY_ID, 0x19e0);
    reg_value &= ~(1 << 0);
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x19e0, reg_value);
    return 0;
}

static int en8801s_phase1_init(struct phy_device *phydev)
{
    unsigned long pbus_data;
    unsigned int pbusAddress;
    u32 reg_value;
    int retry;
    struct mii_bus *mbus = phydev_mdio_bus(phydev);

    msleep(1500);

    pbusAddress = EN8801S_PBUS_DEFAULT_ID;
    retry = MAX_OUI_CHECK;
    while (1) {
        pbus_data = airoha_pbus_read(mbus, pbusAddress, EN8801S_RG_ETHER_PHY_OUI);      /* PHY OUI */
        if (EN8801S_PBUS_OUI == pbus_data) {
            pbus_data = airoha_pbus_read(mbus, pbusAddress, EN8801S_RG_SMI_ADDR);       /* SMI ADDR */
            pbus_data = (pbus_data & 0xffff0000) | (unsigned long)(EN8801S_PBUS_PHY_ID << 8) | (unsigned long)(EN8801S_MDIO_PHY_ID);
            phydev_info(phydev, "SMI_ADDR=%lx (renew)\n", pbus_data);
            airoha_pbus_write(mbus, pbusAddress, EN8801S_RG_SMI_ADDR, pbus_data);
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, EN8801S_RG_BUCK_CTL, 0x03);
            mdelay(10);
            break;
        } else {
            pbusAddress = EN8801S_PBUS_PHY_ID;
        }
        retry --;
        if (0 == retry) {
            phydev_err(phydev, "Probe fail !\n");
            return 0;
        }
    }

    reg_value = (airoha_pbus_read(mbus, EN8801S_PBUS_PHY_ID, EN8801S_RG_LTR_CTL) & 0xfffffffc) | 0x10 | (EN8801S_RX_POLARITY << 1) | EN8801S_TX_POLARITY;
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, EN8801S_RG_LTR_CTL, reg_value);
    mdelay(10);
    reg_value &= 0xffffffef;
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, EN8801S_RG_LTR_CTL, reg_value);

    retry = MAX_RETRY;
    while (1) {
        mdelay(10);
        reg_value = phy_read(phydev, MII_PHYSID2);
        if (reg_value == EN8801S_PHY_ID2) {
            break;    /* wait GPHY ready */
        }
        retry--;
        if (0 == retry) {
            phydev_err(phydev, "Initialize fail !\n");
            return 0;
        }
    }
    /* Software Reset PHY */
    reg_value = phy_read(phydev, MII_BMCR);
    reg_value |= BMCR_RESET;
    phy_write(phydev, MII_BMCR, reg_value);
    retry = MAX_RETRY;
    do {
        mdelay(10);
        reg_value = phy_read(phydev, MII_BMCR);
        retry--;
        if (0 == retry) {
            phydev_err(phydev, "Reset fail !\n");
            return 0;
        }
    } while (reg_value & BMCR_RESET);

    phydev->dev_flags = PHY_STATE_INIT;

    phydev_info(phydev, "Phase1 initialize OK ! (%s)\n", EN8801S_DRIVER_VERSION);
    return 0;
}

static int en8801s_phase2_init(struct phy_device *phydev)
{
    gephy_all_REG_LpiReg1Ch      GPHY_RG_LPI_1C;
    gephy_all_REG_dev1Eh_reg324h GPHY_RG_1E_324;
    gephy_all_REG_dev1Eh_reg012h GPHY_RG_1E_012;
    gephy_all_REG_dev1Eh_reg017h GPHY_RG_1E_017;
    unsigned long pbus_data;
    u32 reg_value;
    int retry;
    struct mii_bus *mbus = phydev_mdio_bus(phydev);

    reg_value = (airoha_pbus_read(mbus, EN8801S_PBUS_PHY_ID, EN8801S_RG_LTR_CTL) & 0xfffffffc) | 0x10 | (EN8801S_RX_POLARITY << 1) | EN8801S_TX_POLARITY;
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, EN8801S_RG_LTR_CTL, reg_value);
    mdelay(10);
    reg_value &= 0xffffffef;
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, EN8801S_RG_LTR_CTL, reg_value);

    pbus_data = airoha_pbus_read(mbus, EN8801S_PBUS_PHY_ID, 0x1690);
    pbus_data |= (1 << 31);
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x1690, pbus_data);

    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x0600, 0x0c000c00);
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x10, 0xD801);
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x0,  0x9140);

    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x0A14, 0x0003);
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x0600, 0x0c000c00);
    /* Set FCM control */
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x1404, 0x004b);
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x140c, 0x0007);

    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x142c, 0x05050505);
    pbus_data = airoha_pbus_read(mbus, EN8801S_PBUS_PHY_ID, 0x1440);
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x1440, pbus_data & ~(1 << 11));
    /* Set GPHY Perfomance*/
    /* Token Ring */
    airoha_tr_reg_write(mbus, RgAddr_PMA_01h,     0x6FB90A);
    airoha_tr_reg_write(mbus, RgAddr_PMA_18h,     0x0E2F00);
    airoha_tr_reg_write(mbus, RgAddr_DSPF_06h,    0x2EBAEF);
    airoha_tr_reg_write(mbus, RgAddr_DSPF_11h,    0x040001);
    airoha_tr_reg_write(mbus, RgAddr_DSPF_03h,    0x000004);
    airoha_tr_reg_write(mbus, RgAddr_DSPF_1Ch,    0x003210);
    airoha_tr_reg_write(mbus, RgAddr_DSPF_14h,    0x00024A);
    airoha_tr_reg_write(mbus, RgAddr_DSPF_0Ch,    0x00704D);
    airoha_tr_reg_write(mbus, RgAddr_DSPF_0Dh,    0x02314F);
    airoha_tr_reg_write(mbus, RgAddr_DSPF_10h,    0x005010);
    airoha_tr_reg_write(mbus, RgAddr_DSPF_0Fh,    0x003028);
    airoha_tr_reg_write(mbus, RgAddr_TR_26h,      0x444444);
    airoha_tr_reg_write(mbus, RgAddr_R1000DEC_15h, 0x0055A0);
    /* CL22 & CL45 */
    phy_write(phydev, 0x1f, 0x03);
    GPHY_RG_LPI_1C.DATA = phy_read(phydev, RgAddr_LpiReg1Ch);
    GPHY_RG_LPI_1C.DataBitField.smi_deton_th = 0x0C;
    phy_write(phydev, RgAddr_LpiReg1Ch, GPHY_RG_LPI_1C.DATA);
    phy_write(phydev, 0x1f, 0x0);
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1E, 0x122, 0xffff);
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1E, 0x234, 0x0180);
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1E, 0x238, 0x0120);
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1E, 0x120, 0x9014);
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1E, 0x239, 0x0117);
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1E, 0x14A, 0xEE20);
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1E, 0x19B, 0x0111);
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1F, 0x268, 0x07F4);

    airoha_cl45_read(mbus, EN8801S_MDIO_PHY_ID, 0x1E, 0x324, &reg_value);
    GPHY_RG_1E_324.DATA = (u16)reg_value;
    GPHY_RG_1E_324.DataBitField.smi_det_deglitch_off = 0;
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1E, 0x324, (u32)GPHY_RG_1E_324.DATA);
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1E, 0x19E, 0xC2);
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1E, 0x013, 0x0);

    /* EFUSE */
    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x1C08, 0x40000040);
    retry = MAX_RETRY;
    while (0 != retry) {
        mdelay(1);
        reg_value = airoha_pbus_read(mbus, EN8801S_PBUS_PHY_ID, 0x1C08);
        if ((reg_value & (1 << 30)) == 0) {
            break;
        }
        retry--;
    }
    reg_value = airoha_pbus_read(mbus, EN8801S_PBUS_PHY_ID, 0x1C38);          /* RAW#2 */
    GPHY_RG_1E_012.DataBitField.da_tx_i2mpb_a_tbt = reg_value & 0x03f;
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1E, 0x12, (u32)GPHY_RG_1E_012.DATA);
    GPHY_RG_1E_017.DataBitField.da_tx_i2mpb_b_tbt = (reg_value >> 8) & 0x03f;
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1E, 0x12, (u32)GPHY_RG_1E_017.DATA);

    airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x1C08, 0x40400040);
    retry = MAX_RETRY;
    while (0 != retry) {
        mdelay(1);
        reg_value = airoha_pbus_read(mbus, EN8801S_PBUS_PHY_ID, 0x1C08);
        if ((reg_value & (1 << 30)) == 0) {
            break;
        }
        retry--;
    }
    reg_value = airoha_pbus_read(mbus, EN8801S_PBUS_PHY_ID, 0x1C30);          /* RAW#16 */
    GPHY_RG_1E_324.DataBitField.smi_det_deglitch_off = (reg_value >> 12) & 0x01;
    airoha_cl45_write(mbus, EN8801S_MDIO_PHY_ID, 0x1E, 0x324, (u32)GPHY_RG_1E_324.DATA);

    en8801s_led_init(phydev);

    phydev_info(phydev, "Phase2 initialize OK !\n");
    return 0;
}

static int en8801s_read_status(struct phy_device *phydev)
{
    int ret, preSpeed = phydev->speed;
    struct mii_bus *mbus = phydev_mdio_bus(phydev);
    u32 reg_value;

    ret = genphy_read_status(phydev);
    if (LINK_DOWN == phydev->link) preSpeed = phydev->speed = 0;

    if (phydev->dev_flags == PHY_STATE_PROCESS) {
        en8801s_phy_process(phydev);
        phydev->dev_flags = PHY_STATE_DONE;
    }

    if ((preSpeed != phydev->speed) && (LINK_UP == phydev->link)) {
        preSpeed = phydev->speed;

        if (phydev->dev_flags == PHY_STATE_INIT) {
            en8801s_phase2_init(phydev);
            phydev->dev_flags = PHY_STATE_PROCESS;
        }

        if (preSpeed == SPEED_10) {
            reg_value = airoha_pbus_read(mbus, EN8801S_PBUS_PHY_ID, 0x1694);
            reg_value |= (1 << 31);
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x1694, reg_value);
            phydev->dev_flags = PHY_STATE_PROCESS;
        } else {
            reg_value = airoha_pbus_read(mbus, EN8801S_PBUS_PHY_ID, 0x1694);
            reg_value &= ~(1 << 31);
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x1694, reg_value);
            phydev->dev_flags = PHY_STATE_PROCESS;
        }

        airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x0600, 0x0c000c00);
        if (SPEED_1000 == preSpeed) {
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x10, 0xD801);
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x0,  0x9140);

            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x0A14, 0x0003);
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x0600, 0x0c000c00);
            mdelay(2);      /* delay 2 ms */
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x1404, 0x004b);
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x140c, 0x0007);
        } else if (SPEED_100 == preSpeed) {
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x10, 0xD401);
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x0,  0x9140);

            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x0A14, 0x0007);
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x0600, 0x0c11);
            mdelay(2);      /* delay 2 ms */
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x1404, 0x0027);
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x140c, 0x0007);
        } else if (SPEED_10 == preSpeed) {
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x10, 0xD001);
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x0,  0x9140);

            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x0A14, 0x000b);
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x0600, 0x0c11);
            mdelay(2);      /* delay 2 ms */
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x1404, 0x0027);
            airoha_pbus_write(mbus, EN8801S_PBUS_PHY_ID, 0x140c, 0x0007);
        }
    }
    return ret;
}

static struct phy_driver Airoha_driver[] = {
    {
        .phy_id         = EN8801S_PHY_ID,
        .name           = "Airoha EN8801S",
        .phy_id_mask    = 0x0ffffff0,
        .features       = PHY_GBIT_FEATURES,
        .config_init    = en8801s_phase1_init,
        .config_aneg    = genphy_config_aneg,
        .read_status    = en8801s_read_status,
        .suspend        = genphy_suspend,
        .resume         = genphy_resume,
    }
};

module_phy_driver(Airoha_driver);

static struct mdio_device_id __maybe_unused Airoha_tbl[] = {
    { EN8801S_PHY_ID, 0x0ffffff0 },
    { }
};

MODULE_DEVICE_TABLE(mdio, Airoha_tbl);
