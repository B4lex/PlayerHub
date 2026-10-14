#include "text/TextTables.h"

#include <array>

namespace Text::Tables
{
    namespace
    {
        constexpr std::array kEntries{
            // Settings sections
            Entry{ Id::kSectionConnection, "Conexión" },
            Entry{ Id::kSectionDefaultOutfit, "Atuendo predeterminado" },
            Entry{ Id::kSectionUI, "Interfaz" },
            Entry{ Id::kSectionControls, "Teclas" },
            Entry{ Id::kSectionAdvanced, "Avanzado" },

            // Connection settings
            Entry{ Id::kConnectionDisplayName, "Nombre visible" },
            Entry{ Id::kConnectionDisplayNameComment, "Se muestra a otros jugadores sobre tu personaje y en el chat." },
            Entry{ Id::kConnectionAutoConnect, "Conectar automáticamente" },
            Entry{ Id::kConnectionAutoConnectComment,
                "Se conecta al servidor automáticamente cuando se carga la partida." },
            Entry{ Id::kConnectionCustomServer, "Servidor personalizado" },
            Entry{ Id::kConnectionCustomServerComment,
                "Se conecta al servidor de la URL de abajo en lugar del servidor oficial." },
            Entry{ Id::kConnectionServerEndpoint, "URL del servidor" },
            Entry{ Id::kConnectionServerEndpointComment,
                "URL del servidor personalizado, que empieza por http:// o https://. Solo se usa mientras el servidor "
                "personalizado está activado." },

            // Default outfit settings
            Entry{ Id::kOutfitComment,
                "Aspecto y equipo que ven los demás en lugar de tus recursos modificados que ellos no tienen." },
            Entry{ Id::kOutfitGroupCharacter, "Personaje" },
            Entry{ Id::kOutfitRace, "Raza" },
            Entry{ Id::kOutfitSex, "Sexo" },
            Entry{ Id::kOutfitMale, "Hombre" },
            Entry{ Id::kOutfitFemale, "Mujer" },
            Entry{ Id::kOutfitGroupApparel, "Indumentaria" },
            Entry{ Id::kOutfitHead, "Cabeza" },
            Entry{ Id::kOutfitBody, "Cuerpo" },
            Entry{ Id::kOutfitHands, "Manos" },
            Entry{ Id::kOutfitAmulet, "Amuleto" },
            Entry{ Id::kOutfitFeet, "Pies" },
            Entry{ Id::kOutfitShield, "Escudo" },
            Entry{ Id::kOutfitCirclet, "Diadema" },
            Entry{ Id::kOutfitBackpack, "Mochila" },
            Entry{ Id::kOutfitGroupWeapons, "Armas" },
            Entry{ Id::kOutfitSword, "Espada" },
            Entry{ Id::kOutfitDagger, "Daga" },
            Entry{ Id::kOutfitWarAxe, "Hacha de guerra" },
            Entry{ Id::kOutfitMace, "Maza" },
            Entry{ Id::kOutfitGreatsword, "Mandoble" },
            Entry{ Id::kOutfitBattleaxe, "Hacha de batalla o martillo de guerra" },
            Entry{ Id::kOutfitBow, "Arco" },
            Entry{ Id::kOutfitStaff, "Bastón" },
            Entry{ Id::kOutfitCrossbow, "Ballesta" },
            Entry{ Id::kOutfitGroupMiscellaneous, "Varios" },
            Entry{ Id::kOutfitArrow, "Flecha" },
            Entry{ Id::kOutfitBolt, "Virote" },

            // Interface settings
            Entry{ Id::kUIGroupDisplay, "Pantalla" },
            Entry{ Id::kUIFontScale, "Tamaño del texto" },
            Entry{ Id::kUIFontScaleComment,
                "Tamaño del texto en la ventana de ajustes, el chat y el resto de la interfaz multijugador. Las "
                "ventanas crecen y se encogen con él." },
            Entry{ Id::kUIAccentColor, "Color de realce" },
            Entry{ Id::kUIAccentColorComment,
                "Color que marca lo que se puede seleccionar o está seleccionado en la ventana de ajustes y el chat, "
                "en hexadecimal RRGGBB." },
            Entry{ Id::kUIBorderColor, "Color del marco" },
            Entry{ Id::kUIBorderColorComment, "Color del marco alrededor de la ventana de ajustes y el chat." },
            Entry{ Id::kUIBorderVanilla, "Vanilla" },
            Entry{ Id::kUIBorderAccent, "Realce" },
            Entry{ Id::kUIBorderThickness, "Grosor del marco" },
            Entry{ Id::kUIBorderThicknessComment, "Grosor del marco alrededor de la ventana de ajustes y el chat." },
            Entry{ Id::kUIGroupPlayers, "Jugadores" },
            Entry{ Id::kUINameplates, "Nombres sobre los jugadores" },
            Entry{ Id::kUINameplatesComment,
                "Muestra los nombres de otros jugadores sobre su cabeza mientras están a la vista." },
            Entry{ Id::kUINameplateDistance, "Distancia de los nombres" },
            Entry{ Id::kUINameplateDistanceComment,
                "Distancia en unidades del juego dentro de la cual se muestran los nombres sobre los jugadores, unas "
                "70 por metro." },
            Entry{ Id::kUIGroupChat, "Chat" },
            Entry{ Id::kUICloseChatOnSend, "Cerrar al enviar" },
            Entry{ Id::kUICloseChatOnSendComment,
                "Cierra el chat cuando Intro envía una línea. Si no, Intro solo envía y una línea vacía no hace "
                "nada." },
            Entry{ Id::kUIChatHorizontal, "Posición horizontal" },
            Entry{ Id::kUIChatHorizontalComment,
                "Distancia del borde izquierdo del chat al borde izquierdo de la pantalla, como fracción del ancho de "
                "la pantalla. El chat permanece en la pantalla." },
            Entry{ Id::kUIChatVertical, "Posición vertical" },
            Entry{ Id::kUIChatVerticalComment,
                "Distancia del borde inferior del chat al borde inferior de la pantalla, como fracción de la altura de "
                "la pantalla. El chat permanece en la pantalla." },
            Entry{ Id::kUIGroupItems, "Objetos" },
            Entry{ Id::kUIItemPreview, "Vista previa de objetos" },
            Entry{ Id::kUIItemPreviewComment, "Muestra un modelo 3D del objeto señalado en las listas de objetos." },

            // Keybind settings
            Entry{ Id::kControlsOpenSettings, "Abrir ajustes" },
            Entry{ Id::kControlsOpenSettingsComment, "Abre y cierra la ventana de ajustes." },
            Entry{ Id::kControlsOpenChat, "Abrir chat" },
            Entry{ Id::kControlsOpenChatComment, "Abre el chat." },
            Entry{ Id::kControlsManagePlayers, "Gestionar jugadores" },
            Entry{ Id::kControlsManagePlayersComment,
                "Abre la ventana que lista a tus amigos y a los jugadores que has silenciado o hecho desaparecer." },
            Entry{ Id::kControlsAcceptTrade, "Aceptar intercambio" },
            Entry{ Id::kControlsAcceptTradeComment,
                "Acepta la oferta de intercambio de otro jugador mientras espera respuesta." },
            Entry{ Id::kControlsDeclineTrade, "Rechazar intercambio" },
            Entry{ Id::kControlsDeclineTradeComment,
                "Rechaza la oferta de intercambio de otro jugador, o retira la tuya, mientras espera respuesta." },

            // Advanced settings
            Entry{ Id::kAdvancedGroupPerformance, "Rendimiento" },
            Entry{ Id::kAdvancedViewRadius, "Radio de visión" },
            Entry{ Id::kAdvancedViewRadiusComment,
                "Distancia en unidades del juego dentro de la cual se muestran otros jugadores. Se aplica en la "
                "próxima conexión." },
            Entry{ Id::kAdvancedViewRadiusAuto, "Auto" },
            Entry{ Id::kAdvancedSpawnsPerSecond, "Apariciones por segundo" },
            Entry{ Id::kAdvancedSpawnsPerSecondComment,
                "Máximo de jugadores remotos y monturas que aparecen por segundo. Así, muchos jugadores que llegan a "
                "la vez aparecen a lo largo de unos segundos en lugar de en un fotograma largo. 0 hace que aparezcan "
                "todos de golpe." },
            Entry{ Id::kAdvancedSpawnsUnlimited, "Ilimitadas" },
            Entry{ Id::kAdvancedGroupDiagnostics, "Diagnóstico" },
            Entry{ Id::kAdvancedStatusPanel, "Panel de estado" },
            Entry{ Id::kAdvancedStatusPanelComment,
                "Muestra el ping, el tráfico de red y el tiempo por fotograma del plugin en la esquina superior "
                "derecha." },

            // Key names
            Entry{ Id::kKeySpace, "Espacio" },
            Entry{ Id::kKeyEnter, "Intro" },
            Entry{ Id::kKeyCapsLock, "Bloq Mayús" },
            Entry{ Id::kKeyInsert, "Insert" },
            Entry{ Id::kKeyDelete, "Supr" },
            Entry{ Id::kKeyHome, "Inicio" },
            Entry{ Id::kKeyEnd, "Fin" },
            Entry{ Id::kKeyPageUp, "Re Pág" },
            Entry{ Id::kKeyPageDown, "Av Pág" },
            Entry{ Id::kKeyUp, "Arriba" },
            Entry{ Id::kKeyDown, "Abajo" },
            Entry{ Id::kKeyLeft, "Izquierda" },
            Entry{ Id::kKeyRight, "Derecha" },
            Entry{ Id::kKeyPause, "Pausa" },
            Entry{ Id::kKeyScrollLock, "Bloq Despl" },
            Entry{ Id::kKeyNumLock, "Bloq Num" },
            Entry{ Id::kKeyNumpadEnter, "Num Intro" },

            // Settings window
            Entry{ Id::kSettingsBrand, "Multijugador" },
            Entry{ Id::kSettingsDisplayNameHint, "Escribe un nombre visible" },
            Entry{ Id::kSettingsServerEndpointHint, "Escribe la URL del servidor" },
            Entry{ Id::kSettingsServerEndpointInvalid, "URL del servidor no válida." },
            Entry{ Id::kSettingsCustomColor, "Color personalizado" },
            Entry{ Id::kSettingsKeyPrompt, "Pulsa una tecla" },
            Entry{ Id::kSettingsKeyUnbound, "Sin asignar" },
            Entry{ Id::kSettingsKeyReserved, "Esta tecla no se puede asignar" },
            Entry{ Id::kSettingsKeyInUse, "{0} ya está asignada a «{1}»" },
            Entry{ Id::kSettingsDefaults, "Predeterminado" },
            Entry{ Id::kSettingsRevert, "Deshacer" },
            Entry{ Id::kSettingsApply, "Aplicar" },
            Entry{ Id::kSettingsUnapplied, "Cambios sin aplicar" },
            Entry{ Id::kSettingsSaved, "Guardado" },
            Entry{ Id::kSettingsSaveFailed, "No se pudo guardar {}" },

            // Connection controls
            Entry{ Id::kConnectionConnect, "Conectar" },
            Entry{ Id::kConnectionCancel, "Cancelar" },
            Entry{ Id::kConnectionDisconnect, "Desconectar" },
            Entry{ Id::kConnectionConnected, "Conectado" },
            Entry{ Id::kConnectionConnecting, "Conectando..." },
            Entry{ Id::kConnectionDisconnecting, "Desconectando..." },
            Entry{ Id::kConnectionDisconnected, "Desconectado" },
            Entry{ Id::kConnectionDisconnectToRename, "Desconéctate para cambiar el nombre visible" },
            Entry{ Id::kConnectionCancelToRename, "Cancela la conexión para cambiar el nombre visible" },
            Entry{ Id::kConnectionNoEndpoint, "Escribe la URL de un servidor personalizado para conectarte." },
            Entry{ Id::kConnectionShortName,
                "El nombre debe tener al menos {} carácter.|El nombre debe tener al menos {} caracteres." },

            // Server refusals
            Entry{ Id::kRefusalProtocolMismatch, "El servidor usa otra versión del protocolo ({})." },
            Entry{ Id::kRefusalDisplayNameTaken, "Otro jugador ya usa ese nombre visible." },
            Entry{ Id::kRefusalDisplayNameInvalid, "El servidor no acepta ese nombre visible." },
            Entry{ Id::kRefusalSessionReplaced,
                "Tu sesión se conectó desde otra partida, lo que cerró esta conexión. Vuelve a conectarte para "
                "recuperarla." },
            Entry{ Id::kRefusalServerFull, "El servidor está lleno." },
            Entry{ Id::kRefusalTooManyNewSessions,
                "Demasiados jugadores nuevos se han unido a este servidor desde tu dirección IP recientemente." },

            // Status panel
            Entry{ Id::kStatusPing, "Ping" },
            Entry{ Id::kStatusPlugin, "Plugin" },

            // Notices
            Entry{ Id::kNoticeConnected, "Conectado al servidor" },
            Entry{ Id::kNoticeFriendOnline, "{} está en línea" },

            // Display name hint; {0} is the key that opens the settings, {1} the Connection tab
            Entry{ Id::kDisplayNameHint, "Pulsa {0} y escribe un nombre visible en la pestaña {1} para conectarte" },

            // Trade prompt; {} is the other player's name
            Entry{ Id::kTradePromptWaiting, "Esperando a que {} responda a tu oferta de intercambio" },
            Entry{ Id::kTradePromptInvited, "{} quiere intercambiar contigo" },
            Entry{ Id::kTradePromptWithdraw, "Retirar" },
            Entry{ Id::kTradePromptAccept, "Aceptar" },
            Entry{ Id::kTradePromptDecline, "Rechazar" },
            Entry{ Id::kTradePromptSeconds, "{} s" },

            // Chat channels and input; {} is a player's name
            Entry{ Id::kChatGlobal, "Global" },
            Entry{ Id::kChatNearby, "Cercano" },
            Entry{ Id::kChatWhispers, "Susurros" },
            Entry{ Id::kChatTo, "A {}" },
            Entry{ Id::kChatFrom, "De {}" },
            Entry{ Id::kChatGlobalTargetHint, "Enviar a todos los jugadores del servidor." },
            Entry{ Id::kChatNearbyTargetHint, "Enviar a los jugadores cercanos." },
            Entry{ Id::kChatWhisperTargetHint, "Susurrar a {}." },
            Entry{ Id::kChatGlobalFilterHint, "Mensajes para todos los jugadores del servidor." },
            Entry{ Id::kChatNearbyFilterHint, "Mensajes de jugadores cercanos." },
            Entry{ Id::kChatWhispersFilterHint, "Mensajes privados entre tú y otro jugador." },
            Entry{ Id::kChatNoPlayerNamed, "No hay ningún jugador en línea con ese nombre" },
            Entry{ Id::kChatInputHint, "Pulsa Tab para cambiar de canal" },

            // Manage players window
            Entry{ Id::kPlayersTitle, "Jugadores" },
            Entry{ Id::kPlayersFriends, "Amigos" },
            Entry{ Id::kPlayersMuted, "Silenciados" },
            Entry{ Id::kPlayersVanished, "Desaparecidos" },
            Entry{ Id::kPlayersRemoveFriend, "Quitar" },
            Entry{ Id::kPlayersUnmute, "Quitar silencio" },
            Entry{ Id::kPlayersUnvanish, "Hacer aparecer" },
            Entry{ Id::kPlayersNoFriends, "No has añadido amigos en este servidor." },
            Entry{ Id::kPlayersNoMuted, "No has silenciado a nadie en este servidor." },
            Entry{ Id::kPlayersNoVanished, "No has hecho desaparecer a nadie en este servidor." },
            Entry{ Id::kPlayersNoMatch, "Ningún jugador con ese nombre" },
            Entry{ Id::kPlayersOnline, "En línea" },
            Entry{ Id::kPlayersOffline, "Desconectado" },

            // Player menu; {0} is a race, {1} a level
            Entry{ Id::kPlayerMenuLevel, "Nivel {}" },
            Entry{ Id::kPlayerMenuRaceLevel, "{} · Nivel {}" },
            Entry{ Id::kPlayerMenuWhisper, "Susurrar" },
            Entry{ Id::kPlayerMenuAcceptTrade, "Aceptar intercambio" },
            Entry{ Id::kPlayerMenuDeclineTrade, "Rechazar intercambio" },
            Entry{ Id::kPlayerMenuTrade, "Intercambiar" },
            Entry{ Id::kPlayerMenuTradeBusy, "Termina primero tu intercambio actual." },
            Entry{ Id::kPlayerMenuTradeHint, "Ofrécele un intercambio de objetos." },
            Entry{ Id::kPlayerMenuTradeChecking, "Comprobando si está cerca..." },
            Entry{ Id::kPlayerMenuTradeTooFar, "Colócate junto a su personaje para intercambiar." },
            Entry{ Id::kPlayerMenuAddFriend, "Añadir como amigo" },
            Entry{ Id::kPlayerMenuRemoveFriend, "Quitar de amigos" },
            Entry{ Id::kPlayerMenuAddFriendHint, "Mantenlo en tu lista de amigos y sabrás cuándo se conecta." },
            Entry{ Id::kPlayerMenuRemoveFriendHint, "Quítalo de tu lista de amigos." },
            Entry{ Id::kPlayerMenuMute, "Silenciar" },
            Entry{ Id::kPlayerMenuMuteHint, "Oculta sus mensajes en todos los canales." },
            Entry{ Id::kPlayerMenuUnmuteHint, "Vuelve a mostrar sus mensajes." },
            Entry{ Id::kPlayerMenuVanish, "Hacer desaparecer" },
            Entry{ Id::kPlayerMenuVanishHint, "Quita su personaje de tu mundo." },
            Entry{ Id::kPlayerMenuUnvanishHint, "Vuelve a mostrar su personaje en tu mundo." },

            // Chat player list
            Entry{ Id::kChatNotConnected, "No conectado al servidor." },
            Entry{ Id::kChatNoOtherPlayers, "No hay otros jugadores en línea" },
            Entry{ Id::kChatMutedAndVanished, "Silenciado y desaparecido" },

            // Shared controls
            Entry{ Id::kSearchHint, "Buscar" },

            // Trade window; {} is the other player's name unless noted
            Entry{ Id::kTradeTitle, "Intercambio con {}" },
            Entry{ Id::kTradePeerOffer, "Oferta de {}" },
            Entry{ Id::kTradeOwnOffer, "Tu oferta" },
            Entry{ Id::kTradeOfferFull, "(llena)" },
            Entry{ Id::kTradeAccepted, "Aceptada" },
            Entry{ Id::kTradeUninspected,
                "Pasa el cursor sobre los objetos para examinarlos (queda {})|Pasa el cursor sobre los objetos para "
                "examinarlos (quedan {})" },
            Entry{ Id::kTradeCountOf, "de {}" },
            Entry{ Id::kTradeRemoveAll, "Quitar todo" },
            Entry{ Id::kTradeOffer, "Ofrecer" },
            Entry{ Id::kTradeInventory, "Tu inventario" },
            Entry{ Id::kTradeCheckingMods, "Comprobando mods..." },
            Entry{ Id::kTradeNothingToTrade, "No llevas nada que intercambiar." },
            Entry{ Id::kTradeAccept, "Aceptar" },
            Entry{ Id::kTradeCancel, "Cancelar" },
            Entry{ Id::kTradeCategoryAll, "Todo" },
            Entry{ Id::kTradeCategoryWeapons, "Armas" },
            Entry{ Id::kTradeCategoryApparel, "Indumentaria" },
            Entry{ Id::kTradeCategoryAmmunition, "Munición" },
            Entry{ Id::kTradeCategoryPotions, "Pociones" },
            Entry{ Id::kTradeCategoryIngredients, "Ingredientes" },
            Entry{ Id::kTradeCategoryBooks, "Libros" },
            Entry{ Id::kTradeCategoryScrolls, "Pergaminos" },
            Entry{ Id::kTradeCategorySoulGems, "Gemas del alma" },
            Entry{ Id::kTradeCategoryKeys, "Llaves" },
            Entry{ Id::kTradeCategoryMiscellaneous, "Varios" },

            // Item lists and cards
            Entry{ Id::kItemsNoMatch, "Ningún objeto coincide." },
            Entry{ Id::kItemPickerNone, "Ninguno" },
            Entry{ Id::kItemPickerMismatch, "{} no encaja en este ajuste, así que no se usa." },
            Entry{ Id::kItemPickerPluginMissing, "{} no está cargado, así que este objeto no se usa." },
            Entry{ Id::kItemPickerReset, "Restablecer a {}" },
            Entry{ Id::kItemPickerNotLoaded, "{} (sin cargar)" },
            Entry{ Id::kItemPickerNotReady, "Los objetos aparecen cuando el juego ha cargado sus datos." },
            Entry{ Id::kItemPickerSearchHint, "Buscar por nombre o plugin" },
            Entry{ Id::kItemCardDamage, "DAÑO" },
            Entry{ Id::kItemCardArmor, "ARMADURA" },
            Entry{ Id::kItemCardWeight, "PESO" },
            Entry{ Id::kItemCardValue, "VALOR" },
            Entry{ Id::kItemCardCharge, "Carga {} / {}" },
            Entry{ Id::kItemPreviewUnavailable, "Sin vista previa" },
            Entry{ Id::kItemPreviewLoading, "Cargando..." },

            // Trade notices and reasons; {} is the other player's name unless noted
            Entry{ Id::kListSeparator, ", " },
            Entry{ Id::kNotConnected, "No conectado al servidor" },
            Entry{ Id::kTradeNothing, "nada" },
            Entry{ Id::kTradeBlockQuest, "Lo necesita una misión" },
            Entry{ Id::kTradeBlockPlayerMade, "Lo has creado tú, así que otras partidas no lo tienen" },
            Entry{ Id::kTradeBlockUnreferenceable, "No se puede describir a otra partida" },
            Entry{ Id::kTradeRejectMissingPlugin, "{0} no tiene instalado {1}" },
            Entry{ Id::kTradeRejectPluginVersion, "{0} tiene otra versión de {1}" },
            Entry{ Id::kTradeRejectMissingEffect, "La partida de {} no tiene el encantamiento" },
            Entry{ Id::kTradeRejectBadEnchantment, "La partida de {} no puede recrear el encantamiento" },
            Entry{ Id::kTradeCannotOffer, "No puedes ofrecer {0}: {1}" },
            Entry{ Id::kTradeBlockerEmpty, "Añade un objeto o espera a que {} añada uno." },
            Entry{ Id::kTradeBlockerSending, "Enviando tu oferta..." },
            Entry{ Id::kTradeBlockerChecking, "Esperando a que la partida de {} compruebe las ofertas..." },
            Entry{ Id::kTradeOfferTooLarge,
                "Tu oferta está llena; estos objetos contienen demasiados datos para enviarlos juntos" },
            Entry{ Id::kTradeItemsMissing, "Ya no tienes {}" },
            Entry{ Id::kTradeCompleted, "Intercambio realizado" },
            Entry{ Id::kTradeClosedBusy, "{} está ocupado con otro intercambio" },
            Entry{ Id::kTradeClosedUnavailable, "{} no puede intercambiar ahora" },
            Entry{ Id::kTradeClosedOutOfRange, "{} está demasiado lejos para intercambiar" },
            Entry{ Id::kTradeClosedEnded, "El intercambio con {} ha terminado" },
            Entry{ Id::kTradeClosedDeclined, "{} ha rechazado tu oferta de intercambio" },
            Entry{ Id::kTradeClosedNoAnswer, "{} no ha respondido a tu oferta de intercambio" },
            Entry{ Id::kTradeClosedOfferExpired, "La oferta de intercambio de {} ha caducado" },
            Entry{ Id::kTradeClosedWithdrawn, "{} ha retirado la oferta de intercambio" },
            Entry{ Id::kTradeClosedCancelled, "{} ha cancelado el intercambio" },
            Entry{ Id::kTradeClosedFailed, "No se pudo abrir el intercambio con {}" },
            Entry{ Id::kTradeFinishCurrent, "Termina primero tu intercambio actual" },
            Entry{ Id::kTradeCannotTradeNow, "No puedes intercambiar ahora" },
            Entry{ Id::kTradeYouDeclined, "Has rechazado la oferta de intercambio de {}" },
            Entry{ Id::kTradeCannotAccept, "No puedes intercambiar en combate ni fuera del mundo" },
            Entry{
                Id::kTradeWindowBlocked, "La ventana de intercambio no se pudo abrir porque había otro menú abierto" },
            Entry{ Id::kTradeCombatDeclined, "Estás en combate\nSe ha rechazado la oferta de intercambio de {}" },
            Entry{ Id::kTradeCombatEnded, "Estás en combate\nEl intercambio con {} ha terminado" },
            Entry{ Id::kTradeGotNoAnswer, "El intercambio con {} no obtuvo respuesta" },

            // Chat messages and failures
            Entry{ Id::kUnknownPlayer, "Desconocido" },
            Entry{ Id::kPlayersSaveFailed,
                "No se pudo guardar tu lista de amigos y de jugadores silenciados y desaparecidos; el cambio dura "
                "hasta que cierres el juego" },
            Entry{ Id::kPlayersVanishLimit,
                "Puedes hacer desaparecer como máximo a {} jugador por servidor|Puedes hacer desaparecer como máximo a "
                "{} jugadores por servidor" },
            Entry{ Id::kChatWhisperNeedsName, "Escribe el nombre de un jugador después de @ y luego tu mensaje" },
            Entry{ Id::kChatRateLimited, "Estás enviando mensajes demasiado rápido" },
            Entry{ Id::kChatNotInWorld, "No estás en el mundo" },
            Entry{ Id::kChatRecipientGone, "Ese jugador ya no está en línea" },
            Entry{ Id::kChatRejected, "El servidor no aceptó tu mensaje" },
            Entry{ Id::kChatInvalidText, "Tu mensaje contiene caracteres que no se pueden enviar" },
            Entry{ Id::kChatPlayerOffline, "{} está desconectado" },
            Entry{ Id::kItemUnknownEffect, "Efecto desconocido" },
        };
    }

    std::span<const Entry> GetSpanish()
    {
        return kEntries;
    }
}
