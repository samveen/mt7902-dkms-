// SPDX-License-Identifier: ISC
/* Copyright (C) 2021 MediaTek Inc. */

#include "mt7902.h"
#include "../dma.h"
#include "../mt76_connac2_mac.h"

int mt7902e_tx_prepare_skb(struct mt7902_mt76_dev *mdev, void *txwi_ptr,
			   enum mt7902_mt76_txq_id qid, struct mt7902_mt76_wcid *wcid,
			   struct ieee80211_sta *sta,
			   struct mt7902_mt76_tx_info *tx_info)
{
	struct mt7902_mt792x_dev *dev = container_of(mdev, struct mt7902_mt792x_dev, mt76);
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(tx_info->skb);
	struct ieee80211_key_conf *key = info->control.hw_key;
	struct mt7902_mt76_connac_hw_txp *txp;
	struct mt7902_mt76_txwi_cache *t;
	int id, pid;
	u8 *txwi = (u8 *)txwi_ptr;

	if (unlikely(tx_info->skb->len <= ETH_HLEN))
		return -EINVAL;

	if (!wcid)
		wcid = &dev->mt76.global_wcid;

	t = (struct mt7902_mt76_txwi_cache *)(txwi + mdev->drv->txwi_size);
	t->skb = tx_info->skb;

	id = mt7902_mt76_token_consume(mdev, &t);
	if (id < 0)
		return id;

	if (sta) {
		struct mt7902_mt792x_sta *msta = (struct mt7902_mt792x_sta *)sta->drv_priv;

		if (time_after(jiffies, msta->last_txs + HZ / 4)) {
			info->flags |= IEEE80211_TX_CTL_REQ_TX_STATUS;
			msta->last_txs = jiffies;
		}
	}

	pid = mt7902_mt76_tx_status_skb_add(mdev, wcid, tx_info->skb);
	mt7902_mt76_connac2_mac_write_txwi(mdev, txwi_ptr, tx_info->skb, wcid, key,
				    pid, qid, 0);

	txp = (struct mt7902_mt76_connac_hw_txp *)(txwi + MT_TXD_SIZE);
	memset(txp, 0, sizeof(struct mt7902_mt76_connac_hw_txp));
	mt7902_mt76_connac_write_hw_txp(mdev, tx_info, txp, id);

	tx_info->skb = NULL;

	return 0;
}

int mt7902e_mac_reset(struct mt7902_mt792x_dev *dev)
{
	int i, err;

	mt7902_mt792xe_mcu_drv_pmctrl(dev);

	mt7902_mt76_connac_free_pending_tx_skbs(&dev->pm, NULL);

	mt7902_mt76_wr(dev, dev->irq_map->host_irq_enable, 0);
	mt7902_mt76_wr(dev, MT_PCIE_MAC_INT_ENABLE, 0x0);

	set_bit(MT76_RESET, &dev->mphy.state);
	set_bit(MT76_MCU_RESET, &dev->mphy.state);
	wake_up(&dev->mt76.mcu.wait);
	skb_queue_purge(&dev->mt76.mcu.res_q);

	mt7902_mt76_txq_schedule_all(&dev->mphy);

	mt7902_mt76_worker_disable(&dev->mt76.tx_worker);
	napi_disable(&dev->mt76.napi[MT_RXQ_MAIN]);
	napi_disable(&dev->mt76.napi[MT_RXQ_MCU]);
	napi_disable(&dev->mt76.napi[MT_RXQ_MCU_WA]);
	napi_disable(&dev->mt76.tx_napi);

	mt7902_mt76_connac2_tx_token_put(&dev->mt76);
	idr_init(&dev->mt76.token);

	mt7902_mt792x_wpdma_reset(dev, true);

	local_bh_disable();
	mt7902_mt76_for_each_q_rx(&dev->mt76, i) {
		napi_enable(&dev->mt76.napi[i]);
		napi_schedule(&dev->mt76.napi[i]);
	}
	local_bh_enable();

	dev->fw_assert = false;
	clear_bit(MT76_MCU_RESET, &dev->mphy.state);

	mt7902_mt76_wr(dev, dev->irq_map->host_irq_enable,
		dev->irq_map->tx.all_complete_mask |
		MT_INT_RX_DONE_ALL | MT_INT_MCU_CMD);
	mt7902_mt76_wr(dev, MT_PCIE_MAC_INT_ENABLE, 0xff);

	err = mt7902e_driver_own(dev);
	if (err)
		goto out;

	err = mt7902_run_firmware(dev);
	if (err)
		goto out;

	err = mt7902_mcu_set_eeprom(dev);
	if (err)
		goto out;

	err = mt7902_mac_init(dev);
	if (err)
		goto out;

	err = __mt7902_start(&dev->phy);
out:
	clear_bit(MT76_RESET, &dev->mphy.state);

	local_bh_disable();
	napi_enable(&dev->mt76.tx_napi);
	napi_schedule(&dev->mt76.tx_napi);
	local_bh_enable();

	mt7902_mt76_worker_enable(&dev->mt76.tx_worker);

	return err;
}
