use crate::inventory::Inventory;
use crate::world_view::{EntityInfo, WorldView};
use gc_core::game_trait::Action;

// ── Hostile mob type IDs (Minecraft 1.20.4) ─────────────────────────────────

const HOSTILE_TYPES: &[u32] = &[
    32,  // Zombie
    33,  // Skeleton
    34,  // Creeper
    35,  // Spider
    38,  // Enderman (neutral until provoked, but track anyway)
    42,  // Witch
    46,  // Phantom
    50,  // Blaze
    51,  // Ghast
    54,  // Wither Skeleton
    58,  // Drowned
    59,  // Pillager
    60,  // Vindicator
    61,  // Ravager
    103, // Warden
];

fn is_hostile(entity_type: u32) -> bool {
    HOSTILE_TYPES.contains(&entity_type)
}

// ── Combat timing ───────────────────────────────────────────────────────────

/// Attack cooldown in ticks for different weapon types.
/// Full charge = max damage. Attacking before cooldown = reduced damage.
fn weapon_cooldown_ticks(item_id: u32) -> u32 {
    match item_id {
        // Swords: 0.625s = 12.5 ticks
        776..=780 => 13,
        // Axes: 1.0s = 20 ticks (except gold: 1.0s)
        746..=750 => 20,
        // Default: 0.25s = 5 ticks (fist)
        _ => 5,
    }
}

// ── Shield usage ────────────────────────────────────────────────────────────

const SHIELD_ITEM_ID: u32 = 442;

// ── Combat system ───────────────────────────────────────────────────────────

/// Manages combat detection and response.
pub struct CombatSystem {
    /// Ticks since last attack (for cooldown tracking).
    ticks_since_attack: u32,
    /// Maximum engagement range (blocks).
    engage_range: f64,
    /// Range at which we start retreating (too close, need space).
    retreat_range: f64,
    /// Whether shield blocking is active.
    shield_active: bool,
}

impl CombatSystem {
    pub fn new() -> Self {
        Self {
            ticks_since_attack: 100, // Start ready to attack
            engage_range: 16.0,
            retreat_range: 2.0,
            shield_active: false,
        }
    }

    /// Set engagement range (how far away we start attacking).
    pub fn set_engage_range(&mut self, range: f64) {
        self.engage_range = range;
    }

    /// Detect hostile mobs within engagement range.
    pub fn detect_hostiles(
        &self,
        world: &WorldView,
        player_x: f64,
        player_y: f64,
        player_z: f64,
    ) -> Vec<EntityInfo> {
        let nearby = world.get_nearby_entities(player_x, player_y, player_z, self.engage_range);
        nearby.into_iter()
            .filter(|e| is_hostile(e.entity_type))
            .collect()
    }

    /// Find the highest-priority target from a list of hostiles.
    /// Priority: closest first, with creepers always highest priority (they explode).
    pub fn select_target<'a>(
        &self,
        hostiles: &'a [EntityInfo],
        player_x: f64,
        player_y: f64,
        player_z: f64,
    ) -> Option<&'a EntityInfo> {
        select_best_target(hostiles, player_x, player_y, player_z)
    }

    /// Generate combat actions for one tick.
    pub fn tick(
        &mut self,
        world: &WorldView,
        inventory: &Inventory,
        player_x: f64,
        player_y: f64,
        player_z: f64,
    ) -> Vec<Action> {
        self.ticks_since_attack += 1;
        let mut actions = Vec::new();

        // Detect hostiles
        let hostiles = self.detect_hostiles(world, player_x, player_y, player_z);
        if hostiles.is_empty() {
            // No threats — lower shield if active
            if self.shield_active {
                self.shield_active = false;
                // Release use item to lower shield
            }
            return actions;
        }

        // Select target — extract needed data to avoid borrow conflict with &mut self
        let target = match select_best_target(&hostiles, player_x, player_y, player_z) {
            Some(t) => t,
            None => return actions,
        };

        let target_id = target.entity_id;
        let target_type = target.entity_type;
        let target_x = target.x;
        let target_y = target.y;
        let target_z = target.z;
        let dist = distance(player_x, player_y, player_z, target_x, target_y, target_z);

        // Shield logic: block if enemy is very close and we have a shield
        let has_shield = inventory.offhand().item_id == SHIELD_ITEM_ID
            || inventory.find_item(SHIELD_ITEM_ID).is_some();

        if dist < self.retreat_range && has_shield && !self.shield_active {
            self.shield_active = true;
            actions.push(Action::UseItem); // raise shield
        }

        // Attack if cooldown is ready and target is in melee range (3 blocks)
        let held = inventory.held_item();
        let cooldown = weapon_cooldown_ticks(held.item_id);

        if dist <= 3.5 && self.ticks_since_attack >= cooldown {
            actions.push(Action::AttackEntity { entity_id: target_id });
            self.ticks_since_attack = 0;

            // Lower shield briefly to attack
            if self.shield_active {
                self.shield_active = false;
            }
        }

        // If target is a creeper and too close, back away
        if target_type == 34 && dist < 4.0 {
            let dx = player_x - target_x;
            let dz = player_z - target_z;
            let len = (dx * dx + dz * dz).sqrt().max(0.001);
            let retreat_x = player_x + (dx / len) * 3.0;
            let retreat_z = player_z + (dz / len) * 3.0;
            actions.push(Action::Sprint(true));
            actions.push(Action::MoveToPosition {
                x: retreat_x,
                y: player_y,
                z: retreat_z,
            });
        }

        actions
    }
}

/// Free function for target selection (avoids borrow conflict with &mut self in tick()).
fn select_best_target<'a>(
    hostiles: &'a [EntityInfo],
    player_x: f64,
    player_y: f64,
    player_z: f64,
) -> Option<&'a EntityInfo> {
    if hostiles.is_empty() {
        return None;
    }
    hostiles.iter().min_by(|a, b| {
        let a_creeper = a.entity_type == 34;
        let b_creeper = b.entity_type == 34;
        if a_creeper && !b_creeper { return std::cmp::Ordering::Less; }
        if !a_creeper && b_creeper { return std::cmp::Ordering::Greater; }
        let dist_a = distance(player_x, player_y, player_z, a.x, a.y, a.z);
        let dist_b = distance(player_x, player_y, player_z, b.x, b.y, b.z);
        dist_a.partial_cmp(&dist_b).unwrap_or(std::cmp::Ordering::Equal)
    })
}

fn distance(x1: f64, y1: f64, z1: f64, x2: f64, y2: f64, z2: f64) -> f64 {
    let dx = x2 - x1;
    let dy = y2 - y1;
    let dz = z2 - z1;
    (dx * dx + dy * dy + dz * dz).sqrt()
}
