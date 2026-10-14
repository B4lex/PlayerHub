#include "text/TextTables.h"

#include <array>

namespace Text::Tables
{
    namespace
    {
        constexpr std::array kEntries{
            // Settings sections
            Entry{ Id::kSectionConnection, "Connexion" },
            Entry{ Id::kSectionDefaultOutfit, "Tenue par défaut" },
            Entry{ Id::kSectionUI, "Interface" },
            Entry{ Id::kSectionControls, "Raccourcis" },
            Entry{ Id::kSectionAdvanced, "Avancé" },

            // Connection settings
            Entry{ Id::kConnectionDisplayName, "Nom affiché" },
            Entry{ Id::kConnectionDisplayNameComment,
                "Visible par les autres joueurs au-dessus de votre personnage et dans le chat." },
            Entry{ Id::kConnectionAutoConnect, "Connexion automatique" },
            Entry{ Id::kConnectionAutoConnectComment,
                "Se connecte automatiquement au serveur une fois la partie chargée." },
            Entry{ Id::kConnectionCustomServer, "Serveur personnalisé" },
            Entry{ Id::kConnectionCustomServerComment,
                "Se connecte au serveur dont l'URL figure ci-dessous au lieu du serveur officiel." },
            Entry{ Id::kConnectionServerEndpoint, "URL du serveur" },
            Entry{ Id::kConnectionServerEndpointComment,
                "URL du serveur personnalisé, commençant par http:// ou https://. Utilisée uniquement quand le serveur "
                "personnalisé est activé." },

            // Default outfit settings
            Entry{ Id::kOutfitComment,
                "Apparence et équipement montrés aux autres joueurs à la place de vos ressources moddées qu'ils ne "
                "possèdent pas." },
            Entry{ Id::kOutfitGroupCharacter, "Personnage" },
            Entry{ Id::kOutfitRace, "Race" },
            Entry{ Id::kOutfitSex, "Sexe" },
            Entry{ Id::kOutfitMale, "Homme" },
            Entry{ Id::kOutfitFemale, "Femme" },
            Entry{ Id::kOutfitGroupApparel, "Tenue" },
            Entry{ Id::kOutfitHead, "Tête" },
            Entry{ Id::kOutfitBody, "Corps" },
            Entry{ Id::kOutfitHands, "Mains" },
            Entry{ Id::kOutfitAmulet, "Amulette" },
            Entry{ Id::kOutfitFeet, "Pieds" },
            Entry{ Id::kOutfitShield, "Bouclier" },
            Entry{ Id::kOutfitCirclet, "Diadème" },
            Entry{ Id::kOutfitBackpack, "Sac à dos" },
            Entry{ Id::kOutfitGroupWeapons, "Armes" },
            Entry{ Id::kOutfitSword, "Épée" },
            Entry{ Id::kOutfitDagger, "Dague" },
            Entry{ Id::kOutfitWarAxe, "Hache de guerre" },
            Entry{ Id::kOutfitMace, "Masse d'armes" },
            Entry{ Id::kOutfitGreatsword, "Épée à deux mains" },
            Entry{ Id::kOutfitBattleaxe, "Hache de bataille ou marteau de guerre" },
            Entry{ Id::kOutfitBow, "Arc" },
            Entry{ Id::kOutfitStaff, "Bâton" },
            Entry{ Id::kOutfitCrossbow, "Arbalète" },
            Entry{ Id::kOutfitGroupMiscellaneous, "Divers" },
            Entry{ Id::kOutfitArrow, "Flèche" },
            Entry{ Id::kOutfitBolt, "Carreau" },

            // Interface settings
            Entry{ Id::kUIGroupDisplay, "Affichage" },
            Entry{ Id::kUIFontScale, "Taille du texte" },
            Entry{ Id::kUIFontScaleComment,
                "Taille du texte de la fenêtre des paramètres, du chat et du reste de l'interface multijoueur. Les "
                "fenêtres s'agrandissent et rétrécissent avec elle." },
            Entry{ Id::kUIAccentColor, "Couleur d'accentuation" },
            Entry{ Id::kUIAccentColorComment,
                "Couleur qui signale ce qui peut être sélectionné ou est sélectionné dans la fenêtre des paramètres et "
                "le chat, en hexadécimal RRGGBB." },
            Entry{ Id::kUIBorderColor, "Couleur du cadre" },
            Entry{ Id::kUIBorderColorComment, "Couleur du cadre autour de la fenêtre des paramètres et du chat." },
            Entry{ Id::kUIBorderVanilla, "Vanilla" },
            Entry{ Id::kUIBorderAccent, "Accentuation" },
            Entry{ Id::kUIBorderThickness, "Épaisseur du cadre" },
            Entry{
                Id::kUIBorderThicknessComment, "Épaisseur du cadre autour de la fenêtre des paramètres et du chat." },
            Entry{ Id::kUIGroupPlayers, "Joueurs" },
            Entry{ Id::kUINameplates, "Noms des joueurs" },
            Entry{ Id::kUINameplatesComment,
                "Affiche le nom des autres joueurs au-dessus de leur tête lorsqu'ils sont en vue." },
            Entry{ Id::kUINameplateDistance, "Distance des noms" },
            Entry{ Id::kUINameplateDistanceComment,
                "Distance en unités de jeu jusqu'à laquelle les noms s'affichent au-dessus des joueurs, environ 70 par "
                "mètre." },
            Entry{ Id::kUIGroupChat, "Chat" },
            Entry{ Id::kUICloseChatOnSend, "Fermer à l'envoi" },
            Entry{ Id::kUICloseChatOnSendComment,
                "Ferme le chat quand Entrée envoie un message. Sinon, Entrée ne fait qu'envoyer, et une ligne vide ne "
                "fait rien." },
            Entry{ Id::kUIChatHorizontal, "Position horizontale" },
            Entry{ Id::kUIChatHorizontalComment,
                "Distance entre le bord gauche du chat et celui de l'écran, en part de la largeur de l'écran. Le chat "
                "reste à l'écran." },
            Entry{ Id::kUIChatVertical, "Position verticale" },
            Entry{ Id::kUIChatVerticalComment,
                "Distance entre le bord inférieur du chat et celui de l'écran, en part de la hauteur de l'écran. Le "
                "chat reste à l'écran." },
            Entry{ Id::kUIGroupItems, "Objets" },
            Entry{ Id::kUIItemPreview, "Aperçu des objets" },
            Entry{ Id::kUIItemPreviewComment, "Affiche un modèle 3D de l'objet survolé dans les listes d'objets." },

            // Keybind settings
            Entry{ Id::kControlsOpenSettings, "Ouvrir les paramètres" },
            Entry{ Id::kControlsOpenSettingsComment, "Ouvre et ferme la fenêtre des paramètres." },
            Entry{ Id::kControlsOpenChat, "Ouvrir le chat" },
            Entry{ Id::kControlsOpenChatComment, "Ouvre le chat." },
            Entry{ Id::kControlsManagePlayers, "Gérer les joueurs" },
            Entry{ Id::kControlsManagePlayersComment,
                "Ouvre la fenêtre qui liste vos amis et les joueurs que vous avez rendus muets ou invisibles." },
            Entry{ Id::kControlsAcceptTrade, "Accepter l'échange" },
            Entry{ Id::kControlsAcceptTradeComment,
                "Accepte la proposition d'échange d'un autre joueur tant qu'elle attend une réponse." },
            Entry{ Id::kControlsDeclineTrade, "Refuser l'échange" },
            Entry{ Id::kControlsDeclineTradeComment,
                "Refuse la proposition d'échange d'un autre joueur, ou retire la vôtre, tant qu'elle attend une "
                "réponse." },

            // Advanced settings
            Entry{ Id::kAdvancedGroupPerformance, "Performances" },
            Entry{ Id::kAdvancedViewRadius, "Rayon de vue" },
            Entry{ Id::kAdvancedViewRadiusComment,
                "Distance en unités de jeu jusqu'à laquelle les autres joueurs sont affichés. S'applique à la "
                "prochaine connexion." },
            Entry{ Id::kAdvancedViewRadiusAuto, "Auto" },
            Entry{ Id::kAdvancedSpawnsPerSecond, "Apparitions par seconde" },
            Entry{ Id::kAdvancedSpawnsPerSecondComment,
                "Nombre maximal de joueurs distants et de montures qui apparaissent par seconde. Quand beaucoup de "
                "joueurs arrivent en même temps, ils apparaissent sur quelques secondes au lieu d'une longue image "
                "figée. 0 les fait tous apparaître d'un coup." },
            Entry{ Id::kAdvancedSpawnsUnlimited, "Illimité" },
            Entry{ Id::kAdvancedGroupDiagnostics, "Diagnostic" },
            Entry{ Id::kAdvancedStatusPanel, "Panneau d'état" },
            Entry{ Id::kAdvancedStatusPanelComment,
                "Affiche le ping, le trafic réseau et le temps par image du module dans le coin supérieur droit." },

            // Key names
            Entry{ Id::kKeySpace, "Espace" },
            Entry{ Id::kKeyEnter, "Entrée" },
            Entry{ Id::kKeyCapsLock, "Verr. Maj" },
            Entry{ Id::kKeyInsert, "Inser" },
            Entry{ Id::kKeyDelete, "Suppr" },
            Entry{ Id::kKeyHome, "Origine" },
            Entry{ Id::kKeyEnd, "Fin" },
            Entry{ Id::kKeyPageUp, "Page préc." },
            Entry{ Id::kKeyPageDown, "Page suiv." },
            Entry{ Id::kKeyUp, "Haut" },
            Entry{ Id::kKeyDown, "Bas" },
            Entry{ Id::kKeyLeft, "Gauche" },
            Entry{ Id::kKeyRight, "Droite" },
            Entry{ Id::kKeyPause, "Pause" },
            Entry{ Id::kKeyScrollLock, "Arrêt défil" },
            Entry{ Id::kKeyNumLock, "Verr. Num" },
            Entry{ Id::kKeyNumpadEnter, "Num Entrée" },

            // Settings window
            Entry{ Id::kSettingsBrand, "Multijoueur" },
            Entry{ Id::kSettingsDisplayNameHint, "Saisissez un nom affiché" },
            Entry{ Id::kSettingsServerEndpointHint, "Saisissez l'URL du serveur" },
            Entry{ Id::kSettingsServerEndpointInvalid, "URL de serveur invalide." },
            Entry{ Id::kSettingsCustomColor, "Couleur personnalisée" },
            Entry{ Id::kSettingsKeyPrompt, "Appuyez sur une touche" },
            Entry{ Id::kSettingsKeyUnbound, "Non attribuée" },
            Entry{ Id::kSettingsKeyReserved, "Cette touche ne peut pas être attribuée" },
            Entry{ Id::kSettingsKeyInUse, "{0} est déjà utilisée par « {1} »" },
            Entry{ Id::kSettingsDefaults, "Par défaut" },
            Entry{ Id::kSettingsRevert, "Annuler" },
            Entry{ Id::kSettingsApply, "Appliquer" },
            Entry{ Id::kSettingsUnapplied, "Modifications non appliquées" },
            Entry{ Id::kSettingsSaved, "Enregistré" },
            Entry{ Id::kSettingsSaveFailed, "Impossible d'enregistrer {}" },

            // Connection controls
            Entry{ Id::kConnectionConnect, "Se connecter" },
            Entry{ Id::kConnectionCancel, "Annuler" },
            Entry{ Id::kConnectionDisconnect, "Se déconnecter" },
            Entry{ Id::kConnectionConnected, "Connecté" },
            Entry{ Id::kConnectionConnecting, "Connexion..." },
            Entry{ Id::kConnectionDisconnecting, "Déconnexion..." },
            Entry{ Id::kConnectionDisconnected, "Déconnecté" },
            Entry{ Id::kConnectionDisconnectToRename, "Déconnectez-vous pour changer le nom affiché" },
            Entry{ Id::kConnectionCancelToRename, "Annulez la connexion pour changer le nom affiché" },
            Entry{ Id::kConnectionNoEndpoint, "Saisissez l'URL d'un serveur personnalisé pour vous connecter." },
            Entry{ Id::kConnectionShortName,
                "Le nom doit comporter au moins {} caractère.|Le nom doit comporter au moins {} caractères." },

            // Server refusals
            Entry{ Id::kRefusalProtocolMismatch, "Le serveur utilise une autre version du protocole ({})." },
            Entry{ Id::kRefusalDisplayNameTaken, "Ce nom affiché est déjà pris par un autre joueur." },
            Entry{ Id::kRefusalDisplayNameInvalid, "Le serveur n'accepte pas ce nom affiché." },
            Entry{ Id::kRefusalSessionReplaced,
                "Votre session s'est connectée depuis une autre partie, ce qui a fermé cette connexion. "
                "Reconnectez-vous pour la reprendre." },
            Entry{ Id::kRefusalServerFull, "Le serveur est plein." },
            Entry{ Id::kRefusalTooManyNewSessions,
                "Trop de nouveaux joueurs ont rejoint ce serveur depuis votre adresse IP récemment." },

            // Status panel
            Entry{ Id::kStatusPing, "Ping" },
            Entry{ Id::kStatusPlugin, "Module" },

            // Notices
            Entry{ Id::kNoticeConnected, "Connecté au serveur" },
            Entry{ Id::kNoticeFriendOnline, "{} est en ligne" },

            // Display name hint; {0} is the key that opens the settings, {1} the Connection tab
            Entry{ Id::kDisplayNameHint,
                "Appuyez sur {0} et saisissez un nom affiché dans l'onglet {1} pour vous connecter" },

            // Trade prompt; {} is the other player's name
            Entry{ Id::kTradePromptWaiting, "En attente de la réponse de {} à votre proposition d'échange" },
            Entry{ Id::kTradePromptInvited, "{} veut faire un échange avec vous" },
            Entry{ Id::kTradePromptWithdraw, "Retirer" },
            Entry{ Id::kTradePromptAccept, "Accepter" },
            Entry{ Id::kTradePromptDecline, "Refuser" },
            Entry{ Id::kTradePromptSeconds, "{} s" },

            // Chat channels and input; {} is a player's name
            Entry{ Id::kChatGlobal, "Global" },
            Entry{ Id::kChatNearby, "Proximité" },
            Entry{ Id::kChatWhispers, "Messages privés" },
            Entry{ Id::kChatTo, "À {}" },
            Entry{ Id::kChatFrom, "De {}" },
            Entry{ Id::kChatGlobalTargetHint, "Envoyer à tous les joueurs du serveur." },
            Entry{ Id::kChatNearbyTargetHint, "Envoyer aux joueurs proches de vous." },
            Entry{ Id::kChatWhisperTargetHint, "Envoyer un message privé à {}." },
            Entry{ Id::kChatGlobalFilterHint, "Messages adressés à tous les joueurs du serveur." },
            Entry{ Id::kChatNearbyFilterHint, "Messages des joueurs proches de vous." },
            Entry{ Id::kChatWhispersFilterHint, "Messages privés entre vous et un autre joueur." },
            Entry{ Id::kChatNoPlayerNamed, "Aucun joueur en ligne ne porte ce nom" },
            Entry{ Id::kChatInputHint, "Appuyez sur Tab pour changer de canal" },

            // Manage players window
            Entry{ Id::kPlayersTitle, "Joueurs" },
            Entry{ Id::kPlayersFriends, "Amis" },
            Entry{ Id::kPlayersMuted, "Muets" },
            Entry{ Id::kPlayersVanished, "Invisibles" },
            Entry{ Id::kPlayersRemoveFriend, "Retirer" },
            Entry{ Id::kPlayersUnmute, "Rétablir le son" },
            Entry{ Id::kPlayersUnvanish, "Rendre visible" },
            Entry{ Id::kPlayersNoFriends, "Vous n'avez ajouté aucun ami sur ce serveur." },
            Entry{ Id::kPlayersNoMuted, "Vous n'avez rendu personne muet sur ce serveur." },
            Entry{ Id::kPlayersNoVanished, "Vous n'avez rendu personne invisible sur ce serveur." },
            Entry{ Id::kPlayersNoMatch, "Aucun joueur ne porte ce nom" },
            Entry{ Id::kPlayersOnline, "En ligne" },
            Entry{ Id::kPlayersOffline, "Hors ligne" },

            // Player menu; {0} is a race, {1} a level
            Entry{ Id::kPlayerMenuLevel, "Niveau {}" },
            Entry{ Id::kPlayerMenuRaceLevel, "{} · Niveau {}" },
            Entry{ Id::kPlayerMenuWhisper, "Message privé" },
            Entry{ Id::kPlayerMenuAcceptTrade, "Accepter l'échange" },
            Entry{ Id::kPlayerMenuDeclineTrade, "Refuser l'échange" },
            Entry{ Id::kPlayerMenuTrade, "Échanger" },
            Entry{ Id::kPlayerMenuTradeBusy, "Terminez d'abord votre échange en cours." },
            Entry{ Id::kPlayerMenuTradeHint, "Lui proposer un échange d'objets." },
            Entry{ Id::kPlayerMenuTradeChecking, "Vérification de la distance..." },
            Entry{ Id::kPlayerMenuTradeTooFar, "Placez-vous près de son personnage pour échanger." },
            Entry{ Id::kPlayerMenuAddFriend, "Ajouter en ami" },
            Entry{ Id::kPlayerMenuRemoveFriend, "Retirer des amis" },
            Entry{ Id::kPlayerMenuAddFriendHint, "L'ajouter à votre liste d'amis et savoir quand il se connecte." },
            Entry{ Id::kPlayerMenuRemoveFriendHint, "Le retirer de votre liste d'amis." },
            Entry{ Id::kPlayerMenuMute, "Rendre muet" },
            Entry{ Id::kPlayerMenuMuteHint, "Masquer ses messages dans tous les canaux." },
            Entry{ Id::kPlayerMenuUnmuteHint, "Afficher de nouveau ses messages." },
            Entry{ Id::kPlayerMenuVanish, "Rendre invisible" },
            Entry{ Id::kPlayerMenuVanishHint, "Retirer son personnage de votre monde." },
            Entry{ Id::kPlayerMenuUnvanishHint, "Afficher de nouveau son personnage dans votre monde." },

            // Chat player list
            Entry{ Id::kChatNotConnected, "Non connecté au serveur." },
            Entry{ Id::kChatNoOtherPlayers, "Aucun autre joueur en ligne" },
            Entry{ Id::kChatMutedAndVanished, "Muet et invisible" },

            // Shared controls
            Entry{ Id::kSearchHint, "Rechercher" },

            // Trade window; {} is the other player's name unless noted
            Entry{ Id::kTradeTitle, "Échange avec {}" },
            Entry{ Id::kTradePeerOffer, "Offre de {}" },
            Entry{ Id::kTradeOwnOffer, "Votre offre" },
            Entry{ Id::kTradeOfferFull, "(pleine)" },
            Entry{ Id::kTradeAccepted, "Acceptée" },
            Entry{ Id::kTradeUninspected,
                "Survolez les objets pour les examiner (encore {})|Survolez les objets pour les examiner (encore {})" },
            Entry{ Id::kTradeCountOf, "sur {}" },
            Entry{ Id::kTradeRemoveAll, "Tout retirer" },
            Entry{ Id::kTradeOffer, "Proposer" },
            Entry{ Id::kTradeInventory, "Votre inventaire" },
            Entry{ Id::kTradeCheckingMods, "Vérification des mods..." },
            Entry{ Id::kTradeNothingToTrade, "Vous n'avez rien à échanger." },
            Entry{ Id::kTradeAccept, "Accepter" },
            Entry{ Id::kTradeCancel, "Annuler" },
            Entry{ Id::kTradeCategoryAll, "Tout" },
            Entry{ Id::kTradeCategoryWeapons, "Armes" },
            Entry{ Id::kTradeCategoryApparel, "Tenues" },
            Entry{ Id::kTradeCategoryAmmunition, "Munitions" },
            Entry{ Id::kTradeCategoryPotions, "Potions" },
            Entry{ Id::kTradeCategoryIngredients, "Ingrédients" },
            Entry{ Id::kTradeCategoryBooks, "Livres" },
            Entry{ Id::kTradeCategoryScrolls, "Parchemins" },
            Entry{ Id::kTradeCategorySoulGems, "Pierres d'âme" },
            Entry{ Id::kTradeCategoryKeys, "Clés" },
            Entry{ Id::kTradeCategoryMiscellaneous, "Divers" },

            // Item lists and cards
            Entry{ Id::kItemsNoMatch, "Aucun objet ne correspond." },
            Entry{ Id::kItemPickerNone, "Aucun" },
            Entry{ Id::kItemPickerMismatch, "{} ne convient pas à ce paramètre et n'est donc pas utilisé." },
            Entry{ Id::kItemPickerPluginMissing, "{} n'est pas chargé, cet objet n'est donc pas utilisé." },
            Entry{ Id::kItemPickerReset, "Rétablir : {}" },
            Entry{ Id::kItemPickerNotLoaded, "{} (non chargé)" },
            Entry{ Id::kItemPickerNotReady, "Les objets sont listés une fois les données du jeu chargées." },
            Entry{ Id::kItemPickerSearchHint, "Rechercher par nom ou par plugin" },
            Entry{ Id::kItemCardDamage, "DÉGÂTS" },
            Entry{ Id::kItemCardArmor, "ARMURE" },
            Entry{ Id::kItemCardWeight, "POIDS" },
            Entry{ Id::kItemCardValue, "VALEUR" },
            Entry{ Id::kItemCardCharge, "Charge {} / {}" },
            Entry{ Id::kItemPreviewUnavailable, "Pas d'aperçu" },
            Entry{ Id::kItemPreviewLoading, "Chargement..." },

            // Trade notices and reasons; {} is the other player's name unless noted
            Entry{ Id::kListSeparator, ", " },
            Entry{ Id::kNotConnected, "Non connecté au serveur" },
            Entry{ Id::kTradeNothing, "rien" },
            Entry{ Id::kTradeBlockQuest, "Une quête en a besoin" },
            Entry{ Id::kTradeBlockPlayerMade, "Fabriqué par vous, les autres parties ne l'ont donc pas" },
            Entry{ Id::kTradeBlockUnreferenceable, "Il ne peut pas être décrit à une autre partie" },
            Entry{ Id::kTradeRejectMissingPlugin, "{0} n'a pas installé {1}" },
            Entry{ Id::kTradeRejectPluginVersion, "{0} a une autre version de {1}" },
            Entry{ Id::kTradeRejectMissingEffect, "La partie de {} n'a pas cet enchantement" },
            Entry{ Id::kTradeRejectBadEnchantment, "La partie de {} ne peut pas recréer cet enchantement" },
            Entry{ Id::kTradeCannotOffer, "Vous ne pouvez pas proposer {0} : {1}" },
            Entry{ Id::kTradeBlockerEmpty, "Ajoutez un objet, ou attendez que {} en ajoute un." },
            Entry{ Id::kTradeBlockerSending, "Envoi de votre offre..." },
            Entry{ Id::kTradeBlockerChecking, "En attente de la vérification des offres par la partie de {}..." },
            Entry{ Id::kTradeOfferTooLarge,
                "Votre offre est pleine : ces objets contiennent trop de données pour être envoyés ensemble" },
            Entry{ Id::kTradeItemsMissing, "Vous n'avez plus {}" },
            Entry{ Id::kTradeCompleted, "Échange réussi" },
            Entry{ Id::kTradeClosedBusy, "{} est déjà occupé par un autre échange" },
            Entry{ Id::kTradeClosedUnavailable, "{} ne peut pas échanger pour le moment" },
            Entry{ Id::kTradeClosedOutOfRange, "{} est trop loin pour échanger" },
            Entry{ Id::kTradeClosedEnded, "L'échange avec {} est terminé" },
            Entry{ Id::kTradeClosedDeclined, "{} a refusé votre proposition d'échange" },
            Entry{ Id::kTradeClosedNoAnswer, "{} n'a pas répondu à votre proposition d'échange" },
            Entry{ Id::kTradeClosedOfferExpired, "La proposition d'échange de {} a expiré" },
            Entry{ Id::kTradeClosedWithdrawn, "{} a retiré sa proposition d'échange" },
            Entry{ Id::kTradeClosedCancelled, "{} a annulé l'échange" },
            Entry{ Id::kTradeClosedFailed, "L'échange avec {} n'a pas pu s'ouvrir" },
            Entry{ Id::kTradeFinishCurrent, "Terminez d'abord votre échange en cours" },
            Entry{ Id::kTradeCannotTradeNow, "Vous ne pouvez pas échanger maintenant" },
            Entry{ Id::kTradeYouDeclined, "Vous avez refusé la proposition d'échange de {}" },
            Entry{ Id::kTradeCannotAccept, "Vous ne pouvez pas échanger en combat ou hors du monde" },
            Entry{ Id::kTradeWindowBlocked,
                "La fenêtre d'échange n'a pas pu s'ouvrir pendant qu'un autre menu était ouvert" },
            Entry{ Id::kTradeCombatDeclined, "Vous êtes en combat\nLa proposition d'échange de {} a été refusée" },
            Entry{ Id::kTradeCombatEnded, "Vous êtes en combat\nL'échange avec {} est terminé" },
            Entry{ Id::kTradeGotNoAnswer, "L'échange avec {} n'a pas reçu de réponse" },

            // Chat messages and failures
            Entry{ Id::kUnknownPlayer, "Inconnu" },
            Entry{ Id::kPlayersSaveFailed,
                "Impossible d'enregistrer votre liste d'amis, de joueurs muets et invisibles ; la modification dure "
                "jusqu'à la fermeture du jeu" },
            Entry{ Id::kPlayersVanishLimit,
                "Vous pouvez rendre invisible au plus {} joueur par serveur|Vous pouvez rendre invisibles au plus {} "
                "joueurs par serveur" },
            Entry{ Id::kChatWhisperNeedsName, "Tapez le nom d'un joueur après @, puis votre message" },
            Entry{ Id::kChatRateLimited, "Vous envoyez des messages trop vite" },
            Entry{ Id::kChatNotInWorld, "Vous n'êtes pas dans le monde" },
            Entry{ Id::kChatRecipientGone, "Ce joueur n'est plus en ligne" },
            Entry{ Id::kChatRejected, "Le serveur n'a pas accepté votre message" },
            Entry{ Id::kChatInvalidText, "Votre message contient des caractères qui ne peuvent pas être envoyés" },
            Entry{ Id::kChatPlayerOffline, "{} est hors ligne" },
            Entry{ Id::kItemUnknownEffect, "Effet inconnu" },
        };
    }

    std::span<const Entry> GetFrench()
    {
        return kEntries;
    }
}
