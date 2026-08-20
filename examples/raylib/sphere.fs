#version 330

in vec3 fragPosition;
in vec2 fragTexCoord;
in vec3 fragNormal;

uniform vec4 colDiffuse;
uniform vec3 viewPos;
uniform vec3 lightDir;

out vec4 finalColor;

void main()
{
    vec2 cell = floor(fragTexCoord*vec2(24.0, 12.0));
    float checker = mod(cell.x + cell.y, 2.0);
    vec3 albedo = mix(vec3(0.85, 0.27, 0.22), vec3(0.96, 0.93, 0.88), checker);
    albedo *= colDiffuse.rgb;

    vec3 n = normalize(fragNormal);
    vec3 l = normalize(-lightDir);
    vec3 v = normalize(viewPos - fragPosition);
    vec3 h = normalize(l + v);

    float diffuse = max(dot(n, l), 0.0);
    float specular = pow(max(dot(n, h), 0.0), 64.0);
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0);

    vec3 ambient = albedo*vec3(0.16, 0.18, 0.24);
    vec3 lit = albedo*diffuse + vec3(0.9, 0.9, 0.85)*specular*0.6;
    lit += vec3(0.20, 0.35, 0.55)*rim;

    finalColor = vec4(ambient + lit, 1.0);
}
