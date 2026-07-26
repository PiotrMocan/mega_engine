#version 450

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform HudPushConstants
{
    uint method;
    uint quality;
    uint fsrAvailable;
    uint padding;
} hud;

uint stringCharacterOff(uint index)
{
    const uint text[7] = uint[7](65u, 65u, 58u, 32u, 79u, 70u, 70u);
    return index < 7u ? text[index] : 32u;
}

uint stringCharacterSmaa(uint index, uint quality)
{
    const uint prefix[9] =
        uint[9](65u, 65u, 58u, 32u, 83u, 77u, 65u, 65u, 32u);
    if (index < 9u)
        return prefix[index];
    index -= 9u;

    if (quality == 0u)
    {
        const uint word[3] = uint[3](76u, 79u, 87u);
        return index < 3u ? word[index] : 32u;
    }
    if (quality == 1u)
    {
        const uint word[6] =
            uint[6](77u, 69u, 68u, 73u, 85u, 77u);
        return index < 6u ? word[index] : 32u;
    }
    if (quality == 2u)
    {
        const uint word[4] = uint[4](72u, 73u, 71u, 72u);
        return index < 4u ? word[index] : 32u;
    }
    const uint word[5] = uint[5](85u, 76u, 84u, 82u, 65u);
    return index < 5u ? word[index] : 32u;
}

uint stringCharacterFsr(uint index, uint quality)
{
    const uint prefix[14] = uint[14](
        65u, 65u, 58u, 32u, 70u, 83u, 82u, 51u,
        32u, 51u, 46u, 49u, 46u, 52u);
    if (index < 14u)
        return prefix[index];
    if (index == 14u)
        return 32u;
    index -= 15u;

    if (quality == 0u)
    {
        const uint word[9] =
            uint[9](78u, 65u, 84u, 73u, 86u, 69u, 45u, 65u, 65u);
        return index < 9u ? word[index] : 32u;
    }
    if (quality == 1u)
    {
        const uint word[7] =
            uint[7](81u, 85u, 65u, 76u, 73u, 84u, 89u);
        return index < 7u ? word[index] : 32u;
    }
    if (quality == 2u)
    {
        const uint word[8] =
            uint[8](66u, 65u, 76u, 65u, 78u, 67u, 69u, 68u);
        return index < 8u ? word[index] : 32u;
    }
    if (quality == 3u)
    {
        const uint word[11] = uint[11](
            80u, 69u, 82u, 70u, 79u, 82u, 77u, 65u, 78u, 67u, 69u);
        return index < 11u ? word[index] : 32u;
    }
    const uint word[17] = uint[17](
        85u, 76u, 84u, 82u, 65u, 45u, 80u, 69u, 82u,
        70u, 79u, 82u, 77u, 65u, 78u, 67u, 69u);
    return index < 17u ? word[index] : 32u;
}

uint controlsCharacter(uint index)
{
    const uint text[37] = uint[37](
        70u, 54u, 32u, 84u, 89u, 80u, 69u,
        32u, 32u, 32u,
        70u, 55u, 32u, 81u, 85u, 65u, 76u, 73u, 84u, 89u, 45u,
        32u, 32u, 32u,
        70u, 56u, 32u, 81u, 85u, 65u, 76u, 73u, 84u, 89u, 43u,
        32u, 32u);
    return index < 37u ? text[index] : 32u;
}

uint glyphRow(uint character, uint row)
{
    uvec2 bits = uvec2(0u);
    switch (character)
    {
    case 43u: bits = uvec2(0x084f9080u, 0u); break;
    case 45u: bits = uvec2(0x000f8000u, 0u); break;
    case 46u: bits = uvec2(0x0c000000u, 6u); break;
    case 48u: bits = uvec2(0x239ace2eu, 14u); break;
    case 49u: bits = uvec2(0x08421184u, 14u); break;
    case 50u: bits = uvec2(0x1041062eu, 31u); break;
    case 51u: bits = uvec2(0x0217043eu, 30u); break;
    case 52u: bits = uvec2(0x05f928c2u, 2u); break;
    case 53u: bits = uvec2(0x021f421fu, 30u); break;
    case 54u: bits = uvec2(0x231f420eu, 14u); break;
    case 55u: bits = uvec2(0x1082083fu, 8u); break;
    case 56u: bits = uvec2(0x2317462eu, 14u); break;
    case 57u: bits = uvec2(0x0217c62eu, 14u); break;
    case 58u: bits = uvec2(0x08401080u, 0u); break;
    case 65u: bits = uvec2(0x231fc62eu, 17u); break;
    case 66u: bits = uvec2(0x231f463eu, 30u); break;
    case 67u: bits = uvec2(0x2108420fu, 15u); break;
    case 68u: bits = uvec2(0x2318c63eu, 30u); break;
    case 69u: bits = uvec2(0x210f421fu, 31u); break;
    case 70u: bits = uvec2(0x210f421fu, 16u); break;
    case 71u: bits = uvec2(0x231bc20fu, 14u); break;
    case 72u: bits = uvec2(0x231fc631u, 17u); break;
    case 73u: bits = uvec2(0x0842108eu, 14u); break;
    case 74u: bits = uvec2(0x25210847u, 12u); break;
    case 75u: bits = uvec2(0x254c5251u, 17u); break;
    case 76u: bits = uvec2(0x21084210u, 31u); break;
    case 77u: bits = uvec2(0x231ad771u, 17u); break;
    case 78u: bits = uvec2(0x2319d731u, 17u); break;
    case 79u: bits = uvec2(0x2318c62eu, 14u); break;
    case 80u: bits = uvec2(0x210f463eu, 16u); break;
    case 81u: bits = uvec2(0x2558c62eu, 13u); break;
    case 82u: bits = uvec2(0x254f463eu, 17u); break;
    case 83u: bits = uvec2(0x0217420fu, 30u); break;
    case 84u: bits = uvec2(0x0842109fu, 4u); break;
    case 85u: bits = uvec2(0x2318c631u, 14u); break;
    case 86u: bits = uvec2(0x1518c631u, 4u); break;
    case 87u: bits = uvec2(0x2b5ac631u, 10u); break;
    case 88u: bits = uvec2(0x22a22a31u, 17u); break;
    case 89u: bits = uvec2(0x08422a31u, 4u); break;
    case 90u: bits = uvec2(0x2082083fu, 31u); break;
    default: break;
    }
    return row < 6u
        ? (bits.x >> (row * 5u)) & 31u
        : bits.y;
}

void main()
{
    const ivec2 origin = ivec2(12, 12);
    const ivec2 size = ivec2(468, 44);
    const ivec2 pixel = ivec2(gl_FragCoord.xy) - origin;
    if (pixel.x < 0 || pixel.y < 0 ||
        pixel.x >= size.x || pixel.y >= size.y)
        discard;

    const int line = pixel.y / 20;
    const ivec2 linePixel = ivec2(pixel.x - 8, pixel.y - line * 20 - 3);
    bool ink = false;
    if (linePixel.x >= 0 && linePixel.y >= 0 && linePixel.y < 14)
    {
        const uint characterIndex = uint(linePixel.x / 12);
        const uint column = uint((linePixel.x % 12) / 2);
        const uint row = uint(linePixel.y / 2);
        uint character = 32u;
        if (line == 0)
        {
            if (hud.method == 0u)
                character = stringCharacterOff(characterIndex);
            else if (hud.method == 1u)
                character = stringCharacterSmaa(characterIndex, hud.quality);
            else
                character = stringCharacterFsr(characterIndex, hud.quality);
        }
        else
        {
            character = controlsCharacter(characterIndex);
        }
        if (column < 5u && row < 7u)
        {
            const uint rowBits = glyphRow(character, row);
            ink = ((rowBits >> (4u - column)) & 1u) != 0u;
        }
    }

    if (ink)
    {
        const vec3 methodColor =
            hud.method == 2u ? vec3(0.45, 0.85, 1.0)
          : hud.method == 1u ? vec3(0.45, 1.0, 0.68)
                             : vec3(0.92, 0.94, 1.0);
        outColor = vec4(line == 0 ? methodColor : vec3(0.82), 1.0);
    }
    else
    {
        outColor = vec4(0.015, 0.022, 0.04, 0.76);
    }
}
