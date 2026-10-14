#include "text/TextTables.h"

#include <array>

namespace Text::Tables
{
    namespace
    {
        constexpr std::array kEntries{
            // Settings sections
            Entry{ Id::kSectionConnection, "Connessione" },
            Entry{ Id::kSectionDefaultOutfit, "Equipaggiamento predefinito" },
            Entry{ Id::kSectionUI, "Interfaccia" },
            Entry{ Id::kSectionControls, "Tasti" },
            Entry{ Id::kSectionAdvanced, "Avanzate" },

            // Connection settings
            Entry{ Id::kConnectionDisplayName, "Nome visualizzato" },
            Entry{ Id::kConnectionDisplayNameComment,
                "Mostrato agli altri giocatori sopra il tuo personaggio e nella chat." },
            Entry{ Id::kConnectionAutoConnect, "Connessione automatica" },
            Entry{ Id::kConnectionAutoConnectComment,
                "Si connette automaticamente al server dopo il caricamento della partita." },
            Entry{ Id::kConnectionCustomServer, "Server personalizzato" },
            Entry{ Id::kConnectionCustomServerComment,
                "Si connette al server all'URL qui sotto invece che al server ufficiale." },
            Entry{ Id::kConnectionServerEndpoint, "URL del server" },
            Entry{ Id::kConnectionServerEndpointComment,
                "URL del server personalizzato, che inizia con http:// o https://. Usato solo quando il server "
                "personalizzato è attivo." },

            // Default outfit settings
            Entry{ Id::kOutfitComment,
                "Aspetto ed equipaggiamento mostrati agli altri al posto delle tue risorse modificate che non "
                "possiedono." },
            Entry{ Id::kOutfitGroupCharacter, "Personaggio" },
            Entry{ Id::kOutfitRace, "Razza" },
            Entry{ Id::kOutfitSex, "Sesso" },
            Entry{ Id::kOutfitMale, "Maschio" },
            Entry{ Id::kOutfitFemale, "Femmina" },
            Entry{ Id::kOutfitGroupApparel, "Abbigliamento" },
            Entry{ Id::kOutfitHead, "Testa" },
            Entry{ Id::kOutfitBody, "Corpo" },
            Entry{ Id::kOutfitHands, "Mani" },
            Entry{ Id::kOutfitAmulet, "Amuleto" },
            Entry{ Id::kOutfitFeet, "Piedi" },
            Entry{ Id::kOutfitShield, "Scudo" },
            Entry{ Id::kOutfitCirclet, "Diadema" },
            Entry{ Id::kOutfitBackpack, "Zaino" },
            Entry{ Id::kOutfitGroupWeapons, "Armi" },
            Entry{ Id::kOutfitSword, "Spada" },
            Entry{ Id::kOutfitDagger, "Pugnale" },
            Entry{ Id::kOutfitWarAxe, "Ascia da guerra" },
            Entry{ Id::kOutfitMace, "Mazza" },
            Entry{ Id::kOutfitGreatsword, "Spadone" },
            Entry{ Id::kOutfitBattleaxe, "Ascia da battaglia o martello da guerra" },
            Entry{ Id::kOutfitBow, "Arco" },
            Entry{ Id::kOutfitStaff, "Bastone" },
            Entry{ Id::kOutfitCrossbow, "Balestra" },
            Entry{ Id::kOutfitGroupMiscellaneous, "Varie" },
            Entry{ Id::kOutfitArrow, "Freccia" },
            Entry{ Id::kOutfitBolt, "Dardo" },

            // Interface settings
            Entry{ Id::kUIGroupDisplay, "Visualizzazione" },
            Entry{ Id::kUIFontScale, "Dimensione testo" },
            Entry{ Id::kUIFontScaleComment,
                "Dimensione del testo nella finestra delle impostazioni, nella chat e nel resto dell'interfaccia "
                "multigiocatore. Le finestre si ingrandiscono e si rimpiccioliscono di conseguenza." },
            Entry{ Id::kUIAccentColor, "Colore di risalto" },
            Entry{ Id::kUIAccentColorComment,
                "Colore che indica ciò che si può selezionare o è selezionato nella finestra delle impostazioni e "
                "nella chat, in esadecimale RRGGBB." },
            Entry{ Id::kUIBorderColor, "Colore cornice" },
            Entry{ Id::kUIBorderColorComment,
                "Colore della cornice attorno alla finestra delle impostazioni e alla chat." },
            Entry{ Id::kUIBorderVanilla, "Vanilla" },
            Entry{ Id::kUIBorderAccent, "Risalto" },
            Entry{ Id::kUIBorderThickness, "Spessore cornice" },
            Entry{ Id::kUIBorderThicknessComment,
                "Spessore della cornice attorno alla finestra delle impostazioni e alla chat." },
            Entry{ Id::kUIGroupPlayers, "Giocatori" },
            Entry{ Id::kUINameplates, "Targhette dei nomi" },
            Entry{ Id::kUINameplatesComment,
                "Mostra i nomi degli altri giocatori sopra la loro testa quando sono in vista." },
            Entry{ Id::kUINameplateDistance, "Distanza targhette" },
            Entry{ Id::kUINameplateDistanceComment,
                "Distanza in unità di gioco entro cui i nomi appaiono sopra i giocatori, circa 70 per metro." },
            Entry{ Id::kUIGroupChat, "Chat" },
            Entry{ Id::kUICloseChatOnSend, "Chiudi all'invio" },
            Entry{ Id::kUICloseChatOnSendComment,
                "Chiude la chat quando Invio manda una riga. Altrimenti Invio si limita a inviare e una riga vuota non "
                "fa nulla." },
            Entry{ Id::kUIChatHorizontal, "Posizione orizzontale" },
            Entry{ Id::kUIChatHorizontalComment,
                "Distanza del bordo sinistro della chat dal bordo sinistro dello schermo, come quota della larghezza "
                "dello schermo. La chat resta sullo schermo." },
            Entry{ Id::kUIChatVertical, "Posizione verticale" },
            Entry{ Id::kUIChatVerticalComment,
                "Distanza del bordo inferiore della chat dal bordo inferiore dello schermo, come quota dell'altezza "
                "dello schermo. La chat resta sullo schermo." },
            Entry{ Id::kUIGroupItems, "Oggetti" },
            Entry{ Id::kUIItemPreview, "Anteprima oggetti" },
            Entry{ Id::kUIItemPreviewComment,
                "Mostra un modello 3D dell'oggetto sotto il cursore negli elenchi di oggetti." },

            // Keybind settings
            Entry{ Id::kControlsOpenSettings, "Apri impostazioni" },
            Entry{ Id::kControlsOpenSettingsComment, "Apre e chiude la finestra delle impostazioni." },
            Entry{ Id::kControlsOpenChat, "Apri chat" },
            Entry{ Id::kControlsOpenChatComment, "Apre la chat." },
            Entry{ Id::kControlsManagePlayers, "Gestisci giocatori" },
            Entry{ Id::kControlsManagePlayersComment,
                "Apre la finestra con i tuoi amici e i giocatori che hai silenziato o fatto sparire." },
            Entry{ Id::kControlsAcceptTrade, "Accetta scambio" },
            Entry{ Id::kControlsAcceptTradeComment,
                "Accetta la proposta di scambio di un altro giocatore mentre attende una risposta." },
            Entry{ Id::kControlsDeclineTrade, "Rifiuta scambio" },
            Entry{ Id::kControlsDeclineTradeComment,
                "Rifiuta la proposta di scambio di un altro giocatore, o ritira la tua, mentre attende una risposta." },

            // Advanced settings
            Entry{ Id::kAdvancedGroupPerformance, "Prestazioni" },
            Entry{ Id::kAdvancedViewRadius, "Raggio visivo" },
            Entry{ Id::kAdvancedViewRadiusComment,
                "Distanza in unità di gioco entro cui vengono mostrati gli altri giocatori. Si applica alla "
                "connessione successiva." },
            Entry{ Id::kAdvancedViewRadiusAuto, "Auto" },
            Entry{ Id::kAdvancedSpawnsPerSecond, "Comparse al secondo" },
            Entry{ Id::kAdvancedSpawnsPerSecondComment,
                "Numero massimo di giocatori remoti e cavalcature che compaiono al secondo. Molti giocatori in arrivo "
                "insieme appaiono così nell'arco di qualche secondo invece che in un lungo fotogramma bloccato. 0 li "
                "fa comparire tutti subito." },
            Entry{ Id::kAdvancedSpawnsUnlimited, "Illimitate" },
            Entry{ Id::kAdvancedGroupDiagnostics, "Diagnostica" },
            Entry{ Id::kAdvancedStatusPanel, "Pannello di stato" },
            Entry{ Id::kAdvancedStatusPanelComment,
                "Mostra il ping, il traffico di rete e il tempo per fotogramma del plugin nell'angolo in alto a "
                "destra." },

            // Key names
            Entry{ Id::kKeySpace, "Spazio" },
            Entry{ Id::kKeyEnter, "Invio" },
            Entry{ Id::kKeyCapsLock, "Bloc Maiusc" },
            Entry{ Id::kKeyInsert, "Ins" },
            Entry{ Id::kKeyDelete, "Canc" },
            Entry{ Id::kKeyHome, "Home" },
            Entry{ Id::kKeyEnd, "Fine" },
            Entry{ Id::kKeyPageUp, "Pag su" },
            Entry{ Id::kKeyPageDown, "Pag giù" },
            Entry{ Id::kKeyUp, "Su" },
            Entry{ Id::kKeyDown, "Giù" },
            Entry{ Id::kKeyLeft, "Sinistra" },
            Entry{ Id::kKeyRight, "Destra" },
            Entry{ Id::kKeyPause, "Pausa" },
            Entry{ Id::kKeyScrollLock, "Bloc Scorr" },
            Entry{ Id::kKeyNumLock, "Bloc Num" },
            Entry{ Id::kKeyNumpadEnter, "Num Invio" },

            // Settings window
            Entry{ Id::kSettingsBrand, "Multigiocatore" },
            Entry{ Id::kSettingsDisplayNameHint, "Inserisci un nome visualizzato" },
            Entry{ Id::kSettingsServerEndpointHint, "Inserisci l'URL del server" },
            Entry{ Id::kSettingsServerEndpointInvalid, "URL del server non valido." },
            Entry{ Id::kSettingsCustomColor, "Colore personalizzato" },
            Entry{ Id::kSettingsKeyPrompt, "Premi un tasto" },
            Entry{ Id::kSettingsKeyUnbound, "Non assegnato" },
            Entry{ Id::kSettingsKeyReserved, "Questo tasto non può essere assegnato" },
            Entry{ Id::kSettingsKeyInUse, "{0} è già usato da «{1}»" },
            Entry{ Id::kSettingsDefaults, "Predefinite" },
            Entry{ Id::kSettingsRevert, "Ripristina" },
            Entry{ Id::kSettingsApply, "Applica" },
            Entry{ Id::kSettingsUnapplied, "Modifiche non applicate" },
            Entry{ Id::kSettingsSaved, "Salvato" },
            Entry{ Id::kSettingsSaveFailed, "Impossibile salvare {}" },

            // Connection controls
            Entry{ Id::kConnectionConnect, "Connetti" },
            Entry{ Id::kConnectionCancel, "Annulla" },
            Entry{ Id::kConnectionDisconnect, "Disconnetti" },
            Entry{ Id::kConnectionConnected, "Connesso" },
            Entry{ Id::kConnectionConnecting, "Connessione..." },
            Entry{ Id::kConnectionDisconnecting, "Disconnessione..." },
            Entry{ Id::kConnectionDisconnected, "Disconnesso" },
            Entry{ Id::kConnectionDisconnectToRename, "Disconnettiti per cambiare il nome visualizzato" },
            Entry{ Id::kConnectionCancelToRename, "Annulla la connessione per cambiare il nome visualizzato" },
            Entry{ Id::kConnectionNoEndpoint, "Inserisci l'URL di un server personalizzato per connetterti." },
            Entry{ Id::kConnectionShortName,
                "Il nome deve contenere almeno {} carattere.|Il nome deve contenere almeno {} caratteri." },

            // Server refusals
            Entry{ Id::kRefusalProtocolMismatch, "Il server usa un'altra versione del protocollo ({})." },
            Entry{ Id::kRefusalDisplayNameTaken, "Il nome visualizzato è già usato da un altro giocatore." },
            Entry{ Id::kRefusalDisplayNameInvalid, "Il server non accetta il nome visualizzato." },
            Entry{ Id::kRefusalSessionReplaced,
                "La tua sessione si è connessa da un'altra partita, che ha chiuso questa connessione. Connettiti di "
                "nuovo per riprenderla." },
            Entry{ Id::kRefusalServerFull, "Il server è pieno." },
            Entry{ Id::kRefusalTooManyNewSessions,
                "Troppi nuovi giocatori si sono uniti di recente a questo server dal tuo indirizzo IP." },

            // Status panel
            Entry{ Id::kStatusPing, "Ping" },
            Entry{ Id::kStatusPlugin, "Plugin" },

            // Notices
            Entry{ Id::kNoticeConnected, "Connesso al server" },
            Entry{ Id::kNoticeFriendOnline, "{} è online" },

            // Display name hint; {0} is the key that opens the settings, {1} the Connection tab
            Entry{
                Id::kDisplayNameHint, "Premi {0} e inserisci un nome visualizzato nella scheda {1} per connetterti" },

            // Trade prompt; {} is the other player's name
            Entry{ Id::kTradePromptWaiting, "In attesa che {} risponda alla tua proposta di scambio" },
            Entry{ Id::kTradePromptInvited, "{} vuole fare uno scambio con te" },
            Entry{ Id::kTradePromptWithdraw, "Ritira" },
            Entry{ Id::kTradePromptAccept, "Accetta" },
            Entry{ Id::kTradePromptDecline, "Rifiuta" },
            Entry{ Id::kTradePromptSeconds, "{} s" },

            // Chat channels and input; {} is a player's name
            Entry{ Id::kChatGlobal, "Globale" },
            Entry{ Id::kChatNearby, "Vicini" },
            Entry{ Id::kChatWhispers, "Sussurri" },
            Entry{ Id::kChatTo, "A {}" },
            Entry{ Id::kChatFrom, "Da {}" },
            Entry{ Id::kChatGlobalTargetHint, "Invia a tutti i giocatori del server." },
            Entry{ Id::kChatNearbyTargetHint, "Invia ai giocatori vicini a te." },
            Entry{ Id::kChatWhisperTargetHint, "Sussurra a {}." },
            Entry{ Id::kChatGlobalFilterHint, "Messaggi a tutti i giocatori del server." },
            Entry{ Id::kChatNearbyFilterHint, "Messaggi dei giocatori vicini a te." },
            Entry{ Id::kChatWhispersFilterHint, "Messaggi privati tra te e un altro giocatore." },
            Entry{ Id::kChatNoPlayerNamed, "Nessun giocatore online con quel nome" },
            Entry{ Id::kChatInputHint, "Premi Tab per cambiare canale" },

            // Manage players window
            Entry{ Id::kPlayersTitle, "Giocatori" },
            Entry{ Id::kPlayersFriends, "Amici" },
            Entry{ Id::kPlayersMuted, "Silenziati" },
            Entry{ Id::kPlayersVanished, "Spariti" },
            Entry{ Id::kPlayersRemoveFriend, "Rimuovi" },
            Entry{ Id::kPlayersUnmute, "Riattiva" },
            Entry{ Id::kPlayersUnvanish, "Fai riapparire" },
            Entry{ Id::kPlayersNoFriends, "Non hai aggiunto amici su questo server." },
            Entry{ Id::kPlayersNoMuted, "Non hai silenziato nessuno su questo server." },
            Entry{ Id::kPlayersNoVanished, "Non hai fatto sparire nessuno su questo server." },
            Entry{ Id::kPlayersNoMatch, "Nessun giocatore con quel nome" },
            Entry{ Id::kPlayersOnline, "Online" },
            Entry{ Id::kPlayersOffline, "Offline" },

            // Player menu; {0} is a race, {1} a level
            Entry{ Id::kPlayerMenuLevel, "Livello {}" },
            Entry{ Id::kPlayerMenuRaceLevel, "{} · Livello {}" },
            Entry{ Id::kPlayerMenuWhisper, "Sussurra" },
            Entry{ Id::kPlayerMenuAcceptTrade, "Accetta scambio" },
            Entry{ Id::kPlayerMenuDeclineTrade, "Rifiuta scambio" },
            Entry{ Id::kPlayerMenuTrade, "Scambia" },
            Entry{ Id::kPlayerMenuTradeBusy, "Concludi prima lo scambio in corso." },
            Entry{ Id::kPlayerMenuTradeHint, "Proponi uno scambio di oggetti." },
            Entry{ Id::kPlayerMenuTradeChecking, "Verifica della distanza..." },
            Entry{ Id::kPlayerMenuTradeTooFar, "Mettiti accanto al suo personaggio per scambiare." },
            Entry{ Id::kPlayerMenuAddFriend, "Aggiungi agli amici" },
            Entry{ Id::kPlayerMenuRemoveFriend, "Rimuovi dagli amici" },
            Entry{ Id::kPlayerMenuAddFriendHint, "Tienilo nella lista amici e scopri quando si connette." },
            Entry{ Id::kPlayerMenuRemoveFriendHint, "Toglilo dalla lista amici." },
            Entry{ Id::kPlayerMenuMute, "Silenzia" },
            Entry{ Id::kPlayerMenuMuteHint, "Nascondi i suoi messaggi in tutti i canali." },
            Entry{ Id::kPlayerMenuUnmuteHint, "Mostra di nuovo i suoi messaggi." },
            Entry{ Id::kPlayerMenuVanish, "Fai sparire" },
            Entry{ Id::kPlayerMenuVanishHint, "Rimuovi il suo personaggio dal tuo mondo." },
            Entry{ Id::kPlayerMenuUnvanishHint, "Mostra di nuovo il suo personaggio nel tuo mondo." },

            // Chat player list
            Entry{ Id::kChatNotConnected, "Non connesso al server." },
            Entry{ Id::kChatNoOtherPlayers, "Nessun altro giocatore online" },
            Entry{ Id::kChatMutedAndVanished, "Silenziato e sparito" },

            // Shared controls
            Entry{ Id::kSearchHint, "Cerca" },

            // Trade window; {} is the other player's name unless noted
            Entry{ Id::kTradeTitle, "Scambio con {}" },
            Entry{ Id::kTradePeerOffer, "Offerta di {}" },
            Entry{ Id::kTradeOwnOffer, "La tua offerta" },
            Entry{ Id::kTradeOfferFull, "(piena)" },
            Entry{ Id::kTradeAccepted, "Accettata" },
            Entry{ Id::kTradeUninspected,
                "Passa sopra gli oggetti per esaminarli (ne manca {})|Passa sopra gli oggetti per esaminarli (ne "
                "mancano {})" },
            Entry{ Id::kTradeCountOf, "di {}" },
            Entry{ Id::kTradeRemoveAll, "Rimuovi tutto" },
            Entry{ Id::kTradeOffer, "Offri" },
            Entry{ Id::kTradeInventory, "Il tuo inventario" },
            Entry{ Id::kTradeCheckingMods, "Verifica delle mod..." },
            Entry{ Id::kTradeNothingToTrade, "Non porti nulla da scambiare." },
            Entry{ Id::kTradeAccept, "Accetta" },
            Entry{ Id::kTradeCancel, "Annulla" },
            Entry{ Id::kTradeCategoryAll, "Tutto" },
            Entry{ Id::kTradeCategoryWeapons, "Armi" },
            Entry{ Id::kTradeCategoryApparel, "Abbigliamento" },
            Entry{ Id::kTradeCategoryAmmunition, "Munizioni" },
            Entry{ Id::kTradeCategoryPotions, "Pozioni" },
            Entry{ Id::kTradeCategoryIngredients, "Ingredienti" },
            Entry{ Id::kTradeCategoryBooks, "Libri" },
            Entry{ Id::kTradeCategoryScrolls, "Pergamene" },
            Entry{ Id::kTradeCategorySoulGems, "Gemme dell'anima" },
            Entry{ Id::kTradeCategoryKeys, "Chiavi" },
            Entry{ Id::kTradeCategoryMiscellaneous, "Varie" },

            // Item lists and cards
            Entry{ Id::kItemsNoMatch, "Nessun oggetto corrisponde." },
            Entry{ Id::kItemPickerNone, "Nessuno" },
            Entry{ Id::kItemPickerMismatch, "{} non è adatto a questa impostazione, quindi non viene usato." },
            Entry{ Id::kItemPickerPluginMissing, "{} non è caricato, quindi questo oggetto non viene usato." },
            Entry{ Id::kItemPickerReset, "Ripristina a {}" },
            Entry{ Id::kItemPickerNotLoaded, "{} (non caricato)" },
            Entry{ Id::kItemPickerNotReady, "Gli oggetti vengono elencati quando il gioco ha caricato i suoi dati." },
            Entry{ Id::kItemPickerSearchHint, "Cerca per nome o plugin" },
            Entry{ Id::kItemCardDamage, "DANNI" },
            Entry{ Id::kItemCardArmor, "ARMATURA" },
            Entry{ Id::kItemCardWeight, "PESO" },
            Entry{ Id::kItemCardValue, "VALORE" },
            Entry{ Id::kItemCardCharge, "Carica {} / {}" },
            Entry{ Id::kItemPreviewUnavailable, "Nessuna anteprima" },
            Entry{ Id::kItemPreviewLoading, "Caricamento..." },

            // Trade notices and reasons; {} is the other player's name unless noted
            Entry{ Id::kListSeparator, ", " },
            Entry{ Id::kNotConnected, "Non connesso al server" },
            Entry{ Id::kTradeNothing, "niente" },
            Entry{ Id::kTradeBlockQuest, "Serve a una missione" },
            Entry{ Id::kTradeBlockPlayerMade, "Creato da te, quindi le altre partite non lo hanno" },
            Entry{ Id::kTradeBlockUnreferenceable, "Non può essere descritto a un'altra partita" },
            Entry{ Id::kTradeRejectMissingPlugin, "{0} non ha installato {1}" },
            Entry{ Id::kTradeRejectPluginVersion, "{0} ha un'altra versione di {1}" },
            Entry{ Id::kTradeRejectMissingEffect, "La partita di {} non ha questo incantamento" },
            Entry{ Id::kTradeRejectBadEnchantment, "La partita di {} non può ricreare questo incantamento" },
            Entry{ Id::kTradeCannotOffer, "Non puoi offrire {0}: {1}" },
            Entry{ Id::kTradeBlockerEmpty, "Aggiungi un oggetto, o attendi che {} ne aggiunga uno." },
            Entry{ Id::kTradeBlockerSending, "Invio della tua offerta..." },
            Entry{ Id::kTradeBlockerChecking, "In attesa che la partita di {} verifichi le offerte..." },
            Entry{ Id::kTradeOfferTooLarge,
                "La tua offerta è piena; questi oggetti contengono troppi dati per essere inviati insieme" },
            Entry{ Id::kTradeItemsMissing, "Non hai più {}" },
            Entry{ Id::kTradeCompleted, "Scambio riuscito" },
            Entry{ Id::kTradeClosedBusy, "{} è impegnato in un altro scambio" },
            Entry{ Id::kTradeClosedUnavailable, "{} non può scambiare in questo momento" },
            Entry{ Id::kTradeClosedOutOfRange, "{} è troppo lontano per scambiare" },
            Entry{ Id::kTradeClosedEnded, "Lo scambio con {} è terminato" },
            Entry{ Id::kTradeClosedDeclined, "{} ha rifiutato la tua proposta di scambio" },
            Entry{ Id::kTradeClosedNoAnswer, "{} non ha risposto alla tua proposta di scambio" },
            Entry{ Id::kTradeClosedOfferExpired, "La proposta di scambio di {} è scaduta" },
            Entry{ Id::kTradeClosedWithdrawn, "{} ha ritirato la proposta di scambio" },
            Entry{ Id::kTradeClosedCancelled, "{} ha annullato lo scambio" },
            Entry{ Id::kTradeClosedFailed, "Non è stato possibile aprire lo scambio con {}" },
            Entry{ Id::kTradeFinishCurrent, "Concludi prima lo scambio in corso" },
            Entry{ Id::kTradeCannotTradeNow, "Non puoi scambiare ora" },
            Entry{ Id::kTradeYouDeclined, "Hai rifiutato la proposta di scambio di {}" },
            Entry{ Id::kTradeCannotAccept, "Non puoi scambiare in combattimento o fuori dal mondo" },
            Entry{ Id::kTradeWindowBlocked,
                "La finestra di scambio non si è potuta aprire mentre era aperto un altro menu" },
            Entry{ Id::kTradeCombatDeclined, "Sei in combattimento\nLa proposta di scambio di {} è stata rifiutata" },
            Entry{ Id::kTradeCombatEnded, "Sei in combattimento\nLo scambio con {} è terminato" },
            Entry{ Id::kTradeGotNoAnswer, "Lo scambio con {} non ha ricevuto risposta" },

            // Chat messages and failures
            Entry{ Id::kUnknownPlayer, "Sconosciuto" },
            Entry{ Id::kPlayersSaveFailed,
                "Impossibile salvare l'elenco di amici e di giocatori silenziati e spariti; la modifica vale fino alla "
                "chiusura del gioco" },
            Entry{ Id::kPlayersVanishLimit,
                "Puoi far sparire al massimo {} giocatore per server|Puoi far sparire al massimo {} giocatori per "
                "server" },
            Entry{ Id::kChatWhisperNeedsName, "Scrivi il nome di un giocatore dopo @, poi il tuo messaggio" },
            Entry{ Id::kChatRateLimited, "Stai inviando messaggi troppo in fretta" },
            Entry{ Id::kChatNotInWorld, "Non sei nel mondo" },
            Entry{ Id::kChatRecipientGone, "Quel giocatore non è più online" },
            Entry{ Id::kChatRejected, "Il server non ha accettato il tuo messaggio" },
            Entry{ Id::kChatInvalidText, "Il tuo messaggio contiene caratteri che non possono essere inviati" },
            Entry{ Id::kChatPlayerOffline, "{} è offline" },
            Entry{ Id::kItemUnknownEffect, "Effetto sconosciuto" },
        };
    }

    std::span<const Entry> GetItalian()
    {
        return kEntries;
    }
}
