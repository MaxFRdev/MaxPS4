# Profondeur et culling dans le rendu Metal

Avant ce changement, le rendu Metal n'avait ni test de profondeur ni culling :
les dessins 3D se recouvraient dans l'ordre d'envoi, et les dessins qui
n'écrivent que la profondeur (plus de 70 000 dans un relevé) étaient ignorés.

## Registres PS4 lus (PM4DrawCollector.hpp)

| Registre | Usage |
| --- | --- |
| `DB_DEPTH_CONTROL` (0xa200) | `Z_ENABLE`, `Z_WRITE_ENABLE`, `ZFUNC` (jamais, <, =, <=, >, !=, >=, toujours) |
| `DB_Z_INFO` (0xa010) | format : 0 = pas de tampon de profondeur |
| `DB_Z_WRITE_BASE` / `DB_Z_READ_BASE` (0xa014 / 0xa012) | adresse du tampon, qui identifie sa texture Metal |
| `DB_DEPTH_SIZE` (0xa016) | taille, en tuiles de 8 pixels |
| `DB_RENDER_CONTROL` (0xa000) bit 0 | effacement de la profondeur par un dessin |
| `DB_DEPTH_CLEAR` (0xa00b) | valeur d'effacement, ramenée dans [0, 1] |
| `PA_SU_SC_MODE_CNTL` (0xa205) | culling des faces avant / arrière, sens de la face avant (`FACE`) |

Un dessin a un tampon de profondeur quand `DB_Z_INFO` a un format, que
l'adresse n'est pas nulle et qu'il teste ou efface la profondeur. Sinon, il
est rendu comme avant : Sonic Mania et Mortal Kombat: Legacy Kollection ne
changent pas tant qu'ils n'activent pas `Z_ENABLE`.

Avec `depthOnlyDraws` (activé par le pont Metal du jeu, pas par le rendu
logiciel), un dessin sans couleur (masque `CB_TARGET_MASK` nul ou pas de
cible) qui écrit ou efface la profondeur est transmis avec `colour = false`,
à la taille du tampon de profondeur.

## Rendu Metal (PM4MetalRenderer.mm)

- Chaque adresse de tampon de profondeur a une texture `Depth32Float`
  (quel que soit le format PS4 ; pas de stencil), créée effacée à
  `DB_DEPTH_CLEAR` et gardée d'un dessin et d'une image à l'autre.
  Utilisée avec une cible d'une autre taille, elle est recréée à cette
  taille en copiant la partie commune.
- Un `MTLDepthStencilState` par fonction et écriture (16 au plus), mis en
  cache ; chaque dessin fixe le sien, même dans une passe partagée.
- Un effacement de profondeur dessine avec la fonction « toujours », écriture
  activée et une plage de profondeur réduite à la valeur d'effacement : les
  pixels couverts reçoivent `DB_DEPTH_CLEAR`.
- Les dessins sans couleur ont une passe sans attachement couleur ; sans
  programme de pixels (adresse 0, le PS nul de Gnm) ou si leur programme ne
  peut pas rejeter de pixels, le pipeline n'a pas de fonction fragment.
- Culling : `setCullMode` et `setFrontFacingWinding` d'après
  `PA_SU_SC_MODE_CNTL`. Les positions à l'écran sont celles de la PS4 (Y
  retourné dans le programme de sommets quand l'échelle Y du viewport est
  positive), donc le sens à l'écran est le même. Les listes de rectangles ne
  sont jamais cullées ; un dessin qui culle les deux faces n'est pas envoyé.
- Le rapport de session ajoute une ligne « Profondeur Metal » : dessins avec
  profondeur, dont sans couleur, dessins entièrement cullés, tampons et
  redimensionnements.

## Tests

- `PM4DrawCollectorTest.cpp` : décodage des registres, dessins sans couleur
  transmis ou ignorés, effacement, absence de tampon sans `Z_ENABLE`.
- `PM4MetalRendererTest.mm` (macOS, vrai GPU) : un triangle proche cache un
  triangle lointain dessiné après lui ; sans test, le dernier gagne ; un
  pré-passage sans couleur ni programme de pixels arrête un triangle plus
  lointain ; un effacement remet la profondeur à 1 ; culling des faces
  arrière puis avant, face avant dans les deux sens, Y retourné, et les deux
  faces cullées.

## Limites

- Pas de stencil, pas de profondeur exportée par le programme de pixels
  (`DB_SHADER_CONTROL.Z_EXPORT_ENABLE`), pas de depth bounds.
- La profondeur reste dans Metal : un jeu qui lit son tampon de profondeur
  comme une texture (ombres, occlusion ambiante) lit la mémoire PS4, qui ne
  la contient pas.
- Cibles couleur : voir « Formats de cible couleur » ci-dessous ; les cibles
  tuilées et les formats entiers ne sont pas pris en charge.
- Le rendu 3D sur iPhone reste à valider avec un vrai jeu.

## Formats de cible couleur

Les jeux 3D dessinent souvent leur scène dans une cible 16 bits flottante
avant de la ramener à l'écran. Avec `otherTargetFormats` (pont Metal du jeu
seulement), le collecteur transmet `CB_COLOR0_INFO` FORMAT et NUMBER_TYPE, et
`PM4MetalRenderer::targetFormat` choisit le format Metal :

| PS4 | Metal |
| --- | --- |
| 8_8_8_8 UNORM / SRGB (swap 0 ou 1), SNORM | RGBA8 / BGRA8, `_sRGB`, Snorm |
| 16_16_16_16, 16_16, 16 FLOAT / UNORM / SNORM | RGBA16, RG16, R16 |
| 32, 32_32, 32_32_32_32 FLOAT | R32, RG32, RGBA32Float (sans mélange) |
| 2_10_10_10 UNORM | RGB10A2Unorm |
| 10_11_11 FLOAT | RG11B10Float |
| 8_8, 8 UNORM / SNORM | RG8, R8 |

Les autres formats (entiers, 5_6_5, échanges de composantes) échouent avec un
message qui les nomme. Un T# de même format à l'adresse d'une cible lit sa
texture Metal (textures natives activées), les calculs copient les cibles
avec leur vraie taille de pixel, et le rapport ne relit que les pixels de 4
octets.
