#pragma once
#include <cstdint>
#include <cmath>

// Native pointer hit-testing and the packaged HUD share these normalized
// rectangles. Outside the button (and explicit confirmation) leave input
// entirely with the original ContainerMenu.
namespace HomeChestHud
{
enum Phase:uint32_t {Hidden,Assign,Release,Confirm,Busy};
enum Action:uint32_t {None=0,Ask=5,Yes=6,No=7,MakeShared=8};
struct Point {float X{},Y{};};
inline bool Valid(Point p)
{return std::isfinite(p.X)&&std::isfinite(p.Y)&&p.X>=0&&p.X<=1&&p.Y>=0&&p.Y<=1;}
inline bool Within(Point p,float l,float t,float r,float b)
{return Valid(p)&&p.X>=l&&p.X<r&&p.Y>=t&&p.Y<b;}
inline uint32_t Hit(uint32_t phase,Point p)
{
    if(phase==Assign||phase==Release)
        return Within(p,.815f,.937f,.985f,.982f)?(phase==Assign?Ask:MakeShared):None;
    if(phase==Confirm)
    {
        if(Within(p,.40f,.55f,.48f,.60f))return Yes;
        if(Within(p,.52f,.55f,.60f,.60f))return No;
    }
    return None;
}
struct Press
{
    uint32_t Token{},Action{};
    bool Captured{};
    bool Down(uint32_t token,uint32_t phase,Point point)
    {
        if(Captured)return true;
        if(!token||!Valid(point))return false;
        const auto hit=Hit(phase,point);
        if(!hit&&phase!=Confirm&&phase!=Busy)return false;
        Token=token;Action=hit;Captured=true;return true;
    }
    bool Up(uint32_t token,uint32_t phase,Point point,uint32_t& action)
    {
        action=None;
        if(!Captured)return false;
        if(Token==token&&Action&&Action==Hit(phase,point))action=Action;
        *this={};return true;
    }
};
}
