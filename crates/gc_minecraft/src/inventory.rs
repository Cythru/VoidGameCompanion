use anyhow::{Context, Result};

// ── Slot data ───────────────────────────────────────────────────────────────

/// A single inventory slot.
#[derive(Debug, Clone)]
pub struct Slot {
    /// Item ID (0 = empty).
    pub item_id: u32,
    /// Stack count.
    pub count: u8,
    /// Damage / metadata value.
    pub damage: u16,
    /// NBT tag data (raw bytes, parsed externally).
    pub nbt: Option<Vec<u8>>,
}

impl Slot {
    pub fn empty() -> Self {
        Self {
            item_id: 0,
            count: 0,
            damage: 0,
            nbt: None,
        }
    }

    pub fn is_empty(&self) -> bool {
        self.item_id == 0 || self.count == 0
    }
}

// ── Inventory layout ────────────────────────────────────────────────────────
// Minecraft inventory:
//   Slots 0-8:   Hotbar
//   Slots 9-35:  Main inventory
//   Slots 36-39: Armor (boots, leggings, chestplate, helmet)
//   Slot 40:     Offhand

const HOTBAR_START: usize = 0;
const HOTBAR_END: usize = 9;
const MAIN_START: usize = 9;
const MAIN_END: usize = 36;
const ARMOR_START: usize = 36;
const ARMOR_END: usize = 40;
const OFFHAND_SLOT: usize = 40;
const TOTAL_SLOTS: usize = 41;

/// Armor slot indices.
#[derive(Debug, Clone, Copy)]
pub enum ArmorSlot {
    Boots      = 36,
    Leggings   = 37,
    Chestplate = 38,
    Helmet     = 39,
}

/// Full player inventory.
pub struct Inventory {
    slots: [Slot; TOTAL_SLOTS],
    selected_hotbar: u8,
}

impl Inventory {
    pub fn new() -> Self {
        Self {
            slots: std::array::from_fn(|_| Slot::empty()),
            selected_hotbar: 0,
        }
    }

    // ── Accessors ───────────────────────────────────────────────────────────

    /// Get a slot by index.
    pub fn get(&self, index: usize) -> Option<&Slot> {
        self.slots.get(index)
    }

    /// Get the currently held item (selected hotbar slot).
    pub fn held_item(&self) -> &Slot {
        &self.slots[self.selected_hotbar as usize]
    }

    /// Get the offhand item.
    pub fn offhand(&self) -> &Slot {
        &self.slots[OFFHAND_SLOT]
    }

    /// Get an armor piece.
    pub fn armor(&self, slot: ArmorSlot) -> &Slot {
        &self.slots[slot as usize]
    }

    /// Currently selected hotbar slot (0-8).
    pub fn selected_slot(&self) -> u8 {
        self.selected_hotbar
    }

    // ── Mutations ───────────────────────────────────────────────────────────

    /// Set a slot's contents (called when server sends window items).
    pub fn set_slot(&mut self, index: usize, slot: Slot) -> Result<()> {
        if index >= TOTAL_SLOTS {
            anyhow::bail!("slot index {} out of range (max {})", index, TOTAL_SLOTS - 1);
        }
        self.slots[index] = slot;
        Ok(())
    }

    /// Select a hotbar slot (0-8).
    pub fn select_hotbar(&mut self, slot: u8) -> Result<()> {
        if slot > 8 {
            anyhow::bail!("hotbar slot must be 0-8, got {}", slot);
        }
        self.selected_hotbar = slot;
        Ok(())
    }

    /// Clear the entire inventory.
    pub fn clear(&mut self) {
        for slot in self.slots.iter_mut() {
            *slot = Slot::empty();
        }
    }

    // ── Queries ─────────────────────────────────────────────────────────────

    /// Find the first slot containing a specific item ID.
    /// Searches hotbar first, then main inventory.
    pub fn find_item(&self, item_id: u32) -> Option<usize> {
        // Search hotbar first (quicker to switch)
        for i in HOTBAR_START..HOTBAR_END {
            if self.slots[i].item_id == item_id && !self.slots[i].is_empty() {
                return Some(i);
            }
        }
        // Then main inventory
        for i in MAIN_START..MAIN_END {
            if self.slots[i].item_id == item_id && !self.slots[i].is_empty() {
                return Some(i);
            }
        }
        None
    }

    /// Count total number of a specific item across all inventory slots.
    pub fn count_item(&self, item_id: u32) -> u32 {
        self.slots.iter()
            .filter(|s| s.item_id == item_id)
            .map(|s| s.count as u32)
            .sum()
    }

    /// Find the first empty slot in the hotbar.
    pub fn find_empty_hotbar(&self) -> Option<usize> {
        (HOTBAR_START..HOTBAR_END).find(|&i| self.slots[i].is_empty())
    }

    /// Find the first empty slot in the main inventory.
    pub fn find_empty_main(&self) -> Option<usize> {
        (MAIN_START..MAIN_END).find(|&i| self.slots[i].is_empty())
    }

    /// Check if inventory is full (no empty slots in hotbar + main).
    pub fn is_full(&self) -> bool {
        self.find_empty_hotbar().is_none() && self.find_empty_main().is_none()
    }

    /// Get all non-empty slots as (index, &Slot) pairs.
    pub fn non_empty_slots(&self) -> Vec<(usize, &Slot)> {
        self.slots.iter()
            .enumerate()
            .filter(|(_, s)| !s.is_empty())
            .collect()
    }

    /// Dump inventory contents for debugging.
    pub fn debug_dump(&self) -> String {
        let mut out = String::new();
        out.push_str(&format!("Selected hotbar: {}\n", self.selected_hotbar));

        out.push_str("Hotbar: ");
        for i in HOTBAR_START..HOTBAR_END {
            let s = &self.slots[i];
            if s.is_empty() {
                out.push_str("[empty] ");
            } else {
                out.push_str(&format!("[{}x{}] ", s.item_id, s.count));
            }
        }
        out.push('\n');

        let main_count = (MAIN_START..MAIN_END)
            .filter(|&i| !self.slots[i].is_empty())
            .count();
        out.push_str(&format!("Main: {}/27 slots used\n", main_count));

        for (i, slot) in [ArmorSlot::Helmet, ArmorSlot::Chestplate, ArmorSlot::Leggings, ArmorSlot::Boots]
            .iter()
            .enumerate()
        {
            let s = &self.slots[*slot as usize];
            let name = ["Helmet", "Chestplate", "Leggings", "Boots"][i];
            if s.is_empty() {
                out.push_str(&format!("{}: empty\n", name));
            } else {
                out.push_str(&format!("{}: id={}\n", name, s.item_id));
            }
        }

        out
    }
}
