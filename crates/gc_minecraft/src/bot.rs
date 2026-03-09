use anyhow::Result;
use gc_core::game_trait::Action;

// ── Bot behavior state machines ─────────────────────────────────────────────

/// High-level bot behaviors, each producing a stream of low-level actions.
#[derive(Debug, Clone)]
pub enum BotBehavior {
    /// Follow a named player, keeping within a distance range.
    FollowPlayer {
        target_name: String,
        min_distance: f64,
        max_distance: f64,
    },
    /// Mine a block at a specific position.
    MineBlock {
        x: i32,
        y: i32,
        z: i32,
    },
    /// Place a block at a specific position.
    PlaceBlock {
        x: i32,
        y: i32,
        z: i32,
        block_id: u32,
    },
    /// Attack a specific entity.
    AttackEntity {
        entity_id: u32,
    },
    /// Eat food from inventory.
    EatFood,
    /// Equip an item from a specific slot.
    EquipItem {
        slot: u16,
    },
    /// Idle — do nothing.
    Idle,
}

/// Result of a single behavior tick.
#[derive(Debug)]
pub enum BehaviorResult {
    /// Behavior produced actions and is still running.
    Continue(Vec<Action>),
    /// Behavior is complete.
    Done,
    /// Behavior failed.
    Failed(String),
}

/// State machine for executing bot behaviors.
pub struct BehaviorExecutor {
    current: BotBehavior,
    tick:    u32,
}

impl BehaviorExecutor {
    pub fn new(behavior: BotBehavior) -> Self {
        Self {
            current: behavior,
            tick: 0,
        }
    }

    pub fn current_behavior(&self) -> &BotBehavior {
        &self.current
    }

    /// Advance the behavior by one tick. Returns actions to execute.
    pub fn tick(
        &mut self,
        player_x: f64,
        player_y: f64,
        player_z: f64,
        world: &crate::world_view::WorldView,
    ) -> BehaviorResult {
        self.tick += 1;

        match &self.current {
            BotBehavior::FollowPlayer { target_name, min_distance, max_distance } => {
                // Look up target position from world view
                if let Some((tx, ty, tz)) = world.get_entity_position_by_name(target_name) {
                    let dx = tx - player_x;
                    let dy = ty - player_y;
                    let dz = tz - player_z;
                    let dist = (dx * dx + dy * dy + dz * dz).sqrt();

                    if dist > *max_distance {
                        // Move towards target
                        let nx = player_x + dx / dist * 0.5;
                        let ny = player_y;
                        let nz = player_z + dz / dist * 0.5;
                        BehaviorResult::Continue(vec![
                            Action::Sprint(true),
                            Action::MoveToPosition { x: nx, y: ny, z: nz },
                        ])
                    } else if dist < *min_distance {
                        // Too close — stop
                        BehaviorResult::Continue(vec![
                            Action::Sprint(false),
                        ])
                    } else {
                        // Good distance — idle
                        BehaviorResult::Continue(vec![])
                    }
                } else {
                    // Target not found
                    BehaviorResult::Continue(vec![])
                }
            }

            BotBehavior::MineBlock { x, y, z } => {
                // Check if block still exists
                if world.is_air(*x, *y, *z) {
                    return BehaviorResult::Done;
                }

                // Start digging (simplified — real impl needs dig sequence packets)
                if self.tick == 1 {
                    BehaviorResult::Continue(vec![
                        Action::BreakBlock { x: *x, y: *y, z: *z },
                    ])
                } else if self.tick > 20 {
                    // Assume digging is done after ~1 second
                    BehaviorResult::Done
                } else {
                    BehaviorResult::Continue(vec![])
                }
            }

            BotBehavior::PlaceBlock { x, y, z, block_id } => {
                if self.tick == 1 {
                    BehaviorResult::Continue(vec![
                        Action::PlaceBlock { x: *x, y: *y, z: *z, block_id: *block_id },
                    ])
                } else {
                    BehaviorResult::Done
                }
            }

            BotBehavior::AttackEntity { entity_id } => {
                // Attack every 10 ticks (0.5s at 20tps = attack cooldown)
                if self.tick % 10 == 1 {
                    BehaviorResult::Continue(vec![
                        Action::AttackEntity { entity_id: *entity_id },
                    ])
                } else {
                    BehaviorResult::Continue(vec![])
                }
            }

            BotBehavior::EatFood => {
                if self.tick == 1 {
                    // Select food slot + use item
                    BehaviorResult::Continue(vec![Action::UseItem])
                } else if self.tick > 32 {
                    // Eating takes ~1.6s = 32 ticks
                    BehaviorResult::Done
                } else {
                    BehaviorResult::Continue(vec![])
                }
            }

            BotBehavior::EquipItem { slot } => {
                if *slot <= 8 {
                    BehaviorResult::Continue(vec![
                        Action::SelectHotbar(*slot as u8),
                    ])
                } else {
                    // Need inventory swap — not implemented yet
                    BehaviorResult::Failed("inventory swap not implemented".into())
                }
            }

            BotBehavior::Idle => {
                BehaviorResult::Continue(vec![])
            }
        }
    }

    /// Replace the current behavior.
    pub fn set_behavior(&mut self, behavior: BotBehavior) {
        self.current = behavior;
        self.tick = 0;
    }
}
