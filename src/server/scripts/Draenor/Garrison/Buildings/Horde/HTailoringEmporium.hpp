////////////////////////////////////////////////////////////////////////////////
//
//  MILLENIUM-STUDIO
//  Copyright 2014-2015 Millenium-studio SARL
//  All Rights Reserved.
//
////////////////////////////////////////////////////////////////////////////////
#ifndef GARRISON_TAILORING_EMPORIUM_HORDE_HPP_GARRISON
#define GARRISON_TAILORING_EMPORIUM_HORDE_HPP_GARRISON

#include "../../GarrisonScriptData.hpp"
#include "../../GarrisonNPC.hpp"
#include "GarrisonMgr.hpp"

namespace MS { namespace Garrison 
{
    //////////////////////////////////////////////////////////////////////////
    /// 79864 - Warra The Weaver                                          ////
    //////////////////////////////////////////////////////////////////////////
    namespace npc_WarraTheWeaverData
    {
        extern InitSequenceFunction FnLevel1;
        extern InitSequenceFunction FnLevel2;
        extern InitSequenceFunction FnLevel3;
    }

    using npc_WarraTheWeaverAI = SimpleSequenceCosmeticScriptAI<&npc_WarraTheWeaverData::FnLevel1, &npc_WarraTheWeaverData::FnLevel2, &npc_WarraTheWeaverData::FnLevel3>;

    class npc_WarraTheWeaver : public CreatureScript
    {
        public:
            /// Constructor
            npc_WarraTheWeaver();

            /// Called when a player opens a gossip dialog with the GameObject.
            /// @p_Player     : Source player instance
            /// @p_Creature   : Target GameObject instance
            virtual bool OnGossipHello(Player * p_Player, Creature * p_Creature) override;
            /// Called when a player selects a gossip item in the creature's gossip menu.
            /// @p_Player   : Source player instance
            /// @p_Creature : Target creature instance
            /// @p_Sender   : Sender menu
            /// @p_Action   : Action
            virtual bool OnGossipSelect(Player * p_Player, Creature * p_Creature, uint32 p_Sender, uint32 p_Action) override;

            /// Called when a CreatureAI object is needed for the creature.
            /// @p_Creature : Target creature instance
            virtual CreatureAI * GetAI(Creature * p_Creature) const override;

    };

    //////////////////////////////////////////////////////////////////////////
    /// 79863 - Turga                                                     ////
    //////////////////////////////////////////////////////////////////////////
    namespace npc_TurgaAIData
    {
        extern InitSequenceFunction FnLevel1;
        extern InitSequenceFunction FnLevel2;
        extern InitSequenceFunction FnLevel3;
    }

    using npc_TurgaAI = SimpleSequenceCosmeticScriptAI<&npc_TurgaAIData::FnLevel1, &npc_TurgaAIData::FnLevel2, &npc_TurgaAIData::FnLevel3>;

    class npc_Turga : public CreatureScript
    {
        public:
            /// Constructor
            npc_Turga();

            /// Called when a player opens a gossip dialog with the GameObject.
            /// @p_Player     : Source player instance
            /// @p_Creature   : Target GameObject instance
            virtual bool OnGossipHello(Player * p_Player, Creature * p_Creature) override;
            /// Called when a player selects a gossip item in the creature's gossip menu.
            /// @p_Player   : Source player instance
            /// @p_Creature : Target creature instance
            /// @p_Sender   : Sender menu
            /// @p_Action   : Action
            virtual bool OnGossipSelect(Player * p_Player, Creature * p_Creature, uint32 p_Sender, uint32 p_Action) override;

            /// Called when a CreatureAI object is needed for the creature.
            /// @p_Creature : Target creature instance
            virtual CreatureAI * GetAI(Creature * p_Creature) const override;

    };

}   ///< namespace Garrison
}   ///< namespace MS

#endif  ///< GARRISON_TAILORING_EMPORIUM_HORDE_HPP_GARRISON